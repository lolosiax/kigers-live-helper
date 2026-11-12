#include "internal.h"

#include <dwmapi.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <utility>

namespace {

SRWLOCK g_message_lock = SRWLOCK_INIT;
char g_message[512] = {0};

void utf8_from_wide(const wchar_t* src, char* dst, int capacity) {
    if (capacity <= 0) return;
    dst[0] = '\0';
    if (src == nullptr || src[0] == L'\0') return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, capacity, nullptr, nullptr);
}

/* UWP 应用关掉后窗口对象还在，只是被 DWM 隐藏了，靠 cloaked 过滤掉。 */
bool is_cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked != 0;
}

void process_name_of(DWORD pid, char* dst, int capacity) {
    if (capacity <= 0) return;
    dst[0] = '\0';
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return;

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        utf8_from_wide(name != nullptr ? name + 1 : path, dst, capacity);
    }
    CloseHandle(process);
}

struct WindowEnumContext {
    std::vector<klive_window_desc> windows;
};

BOOL CALLBACK enum_window_proc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<WindowEnumContext*>(param);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // 自己（控制面板 / 镜像窗口）不进列表
    if (pid == GetCurrentProcessId()) return TRUE;
    if (is_cloaked(hwnd)) return TRUE;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return TRUE;

    klive_window_desc desc{};
    desc.handle = static_cast<int64_t>(reinterpret_cast<intptr_t>(hwnd));
    desc.process_id = static_cast<int32_t>(pid);
    desc.width = width;
    desc.height = height;

    wchar_t buffer[160]{};
    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    utf8_from_wide(buffer, desc.title, sizeof(desc.title));

    buffer[0] = L'\0';
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    utf8_from_wide(buffer, desc.class_name, sizeof(desc.class_name));

    process_name_of(pid, desc.process_name, sizeof(desc.process_name));

    context->windows.push_back(desc);
    return TRUE;
}

}  // namespace

void klive_set_message(const char* fmt, ...) {
    SrwExclusiveLock lock(g_message_lock);
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_message, sizeof(g_message), fmt, args);
    va_end(args);
}

const char* klive_message() {
    return g_message;
}

std::vector<KliveOutput> klive_collect_outputs() {
    std::vector<KliveOutput> outputs;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return outputs;
    }

    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc))) continue;
        // 跳过 Microsoft Basic Render Driver 之类的软件适配器
        if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            if (!desc.AttachedToDesktop) continue;

            KliveOutput entry;
            entry.adapter = adapter;
            entry.output = output;
            entry.desc = desc;
            entry.luid = adapter_desc.AdapterLuid;
            entry.index = static_cast<int32_t>(outputs.size());

            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency >= 20) {
                entry.refresh_hz = static_cast<float>(mode.dmDisplayFrequency);
            }

            utf8_from_wide(desc.DeviceName, entry.name, sizeof(entry.name));
            utf8_from_wide(adapter_desc.Description, entry.adapter_name, sizeof(entry.adapter_name));

            DISPLAY_DEVICEW device{};
            device.cb = sizeof(device);
            if (EnumDisplayDevicesW(desc.DeviceName, 0, &device, 0)) {
                utf8_from_wide(device.DeviceString, entry.friendly_name, sizeof(entry.friendly_name));
            }

            outputs.push_back(std::move(entry));
        }
    }

    return outputs;
}

std::vector<klive_window_desc> klive_collect_windows() {
    WindowEnumContext context;
    EnumWindows(enum_window_proc, reinterpret_cast<LPARAM>(&context));
    return std::move(context.windows);
}
