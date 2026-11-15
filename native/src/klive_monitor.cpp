#include "internal.h"

#include <dwmapi.h>
#include <dxgi1_6.h>

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
    if (!klive_is_capturable_window(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

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

bool klive_is_capturable_window(HWND window) {
    if (window == nullptr || !IsWindow(window)) return false;
    if (!IsWindowVisible(window)) return false;
    if (GetWindowTextLengthW(window) <= 0) return false;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    // 自己（控制面板 / 镜像窗口 / console 插件窗口）不进列表，也不能作为动态采集源
    if (pid == GetCurrentProcessId()) return false;
    if (is_cloaked(window)) return false;

    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    return rect.right > rect.left && rect.bottom > rect.top;
}

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

/**
 * 是否为 HDR 输出。
 *
 * DXGI 用 colorSpace 描述桌面合成格式：PQ/P2020（HDR10）与 scRGB 线性都算 HDR，
 * 其余（sRGB / P2020 SDR）按普通 SDR 处理。
 */
bool is_hdr_color_space(DXGI_COLOR_SPACE_TYPE space) {
    return space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
           space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

/**
 * HDR 模式下系统设定的 SDR 白电平（nits）。
 *
 * Auto HDR / HDR 桌面把 SDR 内容按这个亮度映射进 HDR 域，色调映射必须用同一个
 * 基准把 SDR 白还原成 1.0，否则整幅画面会整体偏亮或偏暗。
 * 旧系统没有 GET_SDR_WHITE_LEVEL 时按 200 nits 估计（Windows 常见默认值）。
 */
float query_sdr_white_nits(const wchar_t* device_name) {
    constexpr float kFallbackNits = 200.0f;

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kFallbackNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
        return kFallbackNits;
    }

    for (UINT32 i = 0; i < path_count; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (wcscmp(source.viewGdiDeviceName, device_name) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS || white.SDRWhiteLevel == 0) {
            return kFallbackNits;
        }
        // SDRWhiteLevel 以 1/1000 为单位，1000 对应 80 nits
        return static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    return kFallbackNits;
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

            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6))) {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                    entry.hdr = is_hdr_color_space(desc1.ColorSpace);
                    entry.max_luminance = desc1.MaxLuminance;
                }
            }
            if (entry.hdr) {
                entry.sdr_white_nits = query_sdr_white_nits(desc.DeviceName);
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
