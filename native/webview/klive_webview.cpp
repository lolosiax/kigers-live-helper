/*
 * WebView2 外壳实现。
 *
 * 线程模型：
 *   - 调用方线程只做参数校验、加载采集 DLL、拉起 UI 线程，然后等初始化结果；
 *   - UI 线程拥有窗口、WebView2 与消息循环，托盘命令也在这一线程处理；
 *   - 采集仍然跑在 klive_capture.dll 自己的 worker 线程上，这里只做转发。
 *
 * 页面资源不走虚拟主机映射，而是由 WebResourceRequested 直接从 web_dir
 * 读文件并显式给出 MIME：`.wasm` 在部分机器上会被系统 MIME 认成
 * application/octet-stream，导致 instantiateStreaming 失败。
 */

#include "klive_webview.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>

#include <WebView2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "klive.h"
#include "nlohmann/json.hpp"

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

constexpr UINT kWmTray = WM_APP + 1;
constexpr UINT kWmStopAndDestroy = WM_APP + 2;
constexpr UINT kWmShowPanel = WM_APP + 3;
constexpr UINT kWmCycleMode = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuShow = 1;
constexpr UINT kMenuStop = 2;
constexpr UINT kMenuExit = 3;
constexpr int kIconResourceId = 101;
constexpr const wchar_t* kWindowClass = L"KliveWebViewWindow";
constexpr const wchar_t* kWindowTitle = L"Kiger\u76f4\u64ad\u52a9\u624b";
// 深色标题栏：Windows 10 2004+ 用 20，1809-1909 用 19
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
constexpr DWORD kDwmwaUseImmersiveDarkModeLegacy = 19;

HINSTANCE ModuleHandle() {
    static HINSTANCE instance = [] {
        HMODULE module = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleHandle),
            &module);
        return reinterpret_cast<HINSTANCE>(module);
    }();
    return instance;
}

// ---------------------------------------------------------------------------
// 日志与错误
// ---------------------------------------------------------------------------

bool LogEnabled() {
    static bool enabled = [] {
        char buffer[8] = {};
        DWORD length = GetEnvironmentVariableA("KLIVE_WEBVIEW_LOG", buffer, sizeof(buffer));
        return length > 0 && buffer[0] != '0';
    }();
    return enabled;
}

void Log(const char* format, ...) {
    if (!LogEnabled()) return;
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    printf("%s\n", buffer);
    fflush(stdout);
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

std::mutex g_error_lock;
std::string g_last_error;

void SetError(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_error_lock);
    g_last_error = text;
    if (!text.empty()) {
        Log("[webview] error: %s", text.c_str());
    }
}

std::string LastErrorText() {
    std::lock_guard<std::mutex> lock(g_error_lock);
    return g_last_error;
}

// ---------------------------------------------------------------------------
// 字符串工具
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const char* text) {
    if (text == nullptr || *text == '\0') return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring result(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return std::string();
    std::string result(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::string FixedToUtf8(const char* data, size_t capacity) {
    size_t length = strnlen(data, capacity);
    return std::string(data, length);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& relative) {
    if (directory.empty()) return relative;
    wchar_t last = directory.back();
    if (last == L'\\' || last == L'/') return directory + relative;
    return directory + L"\\" + relative;
}

// ---------------------------------------------------------------------------
// 用户数据目录
//
// 目录由 keel 插件按 Host 的目录规范解析（ctx.dataDir），这里只接收结果：
// 设置由插件写回 keel 配置，WebView2 profile 放在 data_dir 下的 WebView2/。
// ---------------------------------------------------------------------------

/** `LoadLibraryExW` 的 `LOAD_LIBRARY_SEARCH_*` 只接受绝对路径；
 *  Host 传进来的物化目录可能是相对路径（取决于 `--plugins` 参数）。 */
std::wstring AbsolutePath(const std::wstring& path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring buffer(required, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return path;
    buffer.resize(written);
    return buffer;
}

// ---------------------------------------------------------------------------
// 采集 DLL 动态加载
// ---------------------------------------------------------------------------

struct CaptureApi {
    HMODULE module = nullptr;
    void (*initialize)() = nullptr;
    int32_t (*enumerate_monitors)(klive_monitor_desc*, int32_t) = nullptr;
    int32_t (*enumerate_windows)(klive_window_desc*, int32_t) = nullptr;
    int32_t (*start_mirror)(const klive_mirror_config*) = nullptr;
    int32_t (*stop_mirror)() = nullptr;
    int32_t (*is_running)() = nullptr;
    int32_t (*set_mode)(int32_t) = nullptr;
    int32_t (*get_mode)() = nullptr;
    /* HDR 色调曲线；老 DLL 没有这个导出，允许为空 */
    int32_t (*set_tone_curve)(const float*, int32_t) = nullptr;
    void (*get_stats)(klive_mirror_stats*) = nullptr;
    const char* (*last_message)() = nullptr;
    void (*shutdown)() = nullptr;
};

CaptureApi g_api;
std::mutex g_api_lock;

bool LoadCaptureApi(const std::wstring& native_dir) {
    std::lock_guard<std::mutex> lock(g_api_lock);
    if (g_api.module != nullptr) return true;

    std::wstring path = JoinPath(native_dir, L"klive_capture.dll");
    HMODULE module = LoadLibraryExW(
        path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        SetError("加载 klive_capture.dll 失败（" + WideToUtf8(path) +
                 "，GetLastError=" + std::to_string(GetLastError()) + "）");
        return false;
    }

    auto resolve = [module](const char* name) { return GetProcAddress(module, name); };
    g_api.module = module;
    g_api.initialize = reinterpret_cast<decltype(g_api.initialize)>(resolve("klive_initialize"));
    g_api.enumerate_monitors =
        reinterpret_cast<decltype(g_api.enumerate_monitors)>(resolve("klive_enumerate_monitors"));
    g_api.enumerate_windows =
        reinterpret_cast<decltype(g_api.enumerate_windows)>(resolve("klive_enumerate_windows"));
    g_api.start_mirror =
        reinterpret_cast<decltype(g_api.start_mirror)>(resolve("klive_start_mirror"));
    g_api.stop_mirror =
        reinterpret_cast<decltype(g_api.stop_mirror)>(resolve("klive_stop_mirror"));
    g_api.is_running = reinterpret_cast<decltype(g_api.is_running)>(resolve("klive_is_running"));
    g_api.set_mode = reinterpret_cast<decltype(g_api.set_mode)>(resolve("klive_set_mode"));
    g_api.get_mode = reinterpret_cast<decltype(g_api.get_mode)>(resolve("klive_get_mode"));
    g_api.set_tone_curve =
        reinterpret_cast<decltype(g_api.set_tone_curve)>(resolve("klive_set_tone_curve"));
    g_api.get_stats = reinterpret_cast<decltype(g_api.get_stats)>(resolve("klive_get_stats"));
    g_api.last_message =
        reinterpret_cast<decltype(g_api.last_message)>(resolve("klive_last_message"));
    g_api.shutdown = reinterpret_cast<decltype(g_api.shutdown)>(resolve("klive_shutdown"));

    if (g_api.initialize == nullptr || g_api.enumerate_monitors == nullptr ||
        g_api.enumerate_windows == nullptr || g_api.start_mirror == nullptr ||
        g_api.stop_mirror == nullptr || g_api.is_running == nullptr ||
        g_api.set_mode == nullptr || g_api.get_mode == nullptr ||
        g_api.get_stats == nullptr || g_api.last_message == nullptr) {
        SetError("klive_capture.dll 缺少必要的导出函数");
        FreeLibrary(module);
        g_api = CaptureApi{};
        return false;
    }

    g_api.initialize();
    Log("[webview] klive_capture.dll loaded: %s", WideToUtf8(path).c_str());
    return true;
}

std::string CaptureMessage() {
    if (g_api.last_message == nullptr) return std::string();
    const char* text = g_api.last_message();
    return text == nullptr ? std::string() : std::string(text);
}

// ---------------------------------------------------------------------------
// 会话状态
// ---------------------------------------------------------------------------

std::atomic<bool> g_running{false};
std::atomic<bool> g_exit_process{false};
std::atomic<HWND> g_hwnd{nullptr};
std::mutex g_thread_lock;
std::thread g_thread;

struct UiContext {
    std::wstring native_dir;
    std::wstring web_dir;
    std::wstring data_dir;
    int width = 1120;
    int height = 760;
    int min_width = 920;
    int min_height = 640;
    bool devtools = false;
    void (*open_console)(void*) = nullptr;
    void* open_console_user = nullptr;
    std::string settings_json;
    void (*save_settings)(const char*, void*) = nullptr;
    void* save_settings_user = nullptr;

    std::promise<int32_t> init;
    std::atomic<bool> init_done{false};

    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
};

void FinishInit(UiContext* context, int32_t code) {
    if (context == nullptr || context->init_done.exchange(true)) return;
    try {
        context->init.set_value(code);
    } catch (...) {
        // 等待方已经超时离开，忽略
    }
}

UiContext* g_ui = nullptr;
HWND g_tray_hwnd = nullptr;
HICON g_icon = nullptr;
UINT g_taskbar_created_message = 0;

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------

void FillTrayData(NOTIFYICONDATAW& data, HWND hwnd) {
    data = NOTIFYICONDATAW{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kWmTray;
    data.hIcon = g_icon != nullptr ? g_icon : LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"Kiger\u76f4\u64ad\u52a9\u624b", ARRAYSIZE(data.szTip));
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW data;
    FillTrayData(data, hwnd);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        g_tray_hwnd = hwnd;
    } else {
        Log("[webview] Shell_NotifyIcon(NIM_ADD) failed: %lu", GetLastError());
    }
}

void RemoveTrayIcon() {
    if (g_tray_hwnd == nullptr) return;
    NOTIFYICONDATAW data;
    FillTrayData(data, g_tray_hwnd);
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_tray_hwnd = nullptr;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;

    bool mirroring = g_api.is_running != nullptr && g_api.is_running() != 0;
    AppendMenuW(menu, MF_STRING, kMenuShow, L"\u663e\u793a\u9762\u677f");
    AppendMenuW(
        menu, MF_STRING | (mirroring ? MF_ENABLED : MF_GRAYED), kMenuStop,
        L"\u505c\u6b62\u955c\u50cf");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"\u9000\u51fa");

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void StopMirror() {
    if (g_api.stop_mirror != nullptr) {
        g_api.stop_mirror();
    }
}

// ---------------------------------------------------------------------------
// 鼠标侧键（XBUTTON1/2）全局钩子
//
// 低级鼠标钩子不注入其它进程，也不需要管理员权限；但回调必须足够快，
// 超时会被系统静默摘掉，所以这里只 PostMessage，真正的切档在 UI 线程做。
// 只在镜像运行中吞掉侧键：空闲时保留浏览器 / 资源管理器原本的前进后退。
// ---------------------------------------------------------------------------

/*
 * 钩子跑在独立线程上：低级钩子回调超时会被系统静默摘掉，而 UI 线程要跑
 * WebView2 回调、启停镜像（stop 会 join worker），不能保证 300ms 内回消息。
 */
HHOOK g_mouse_hook = nullptr;
std::thread g_hook_thread;
std::atomic<DWORD> g_hook_thread_id{0};
std::atomic<bool> g_hook_ready{false};

LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && (wparam == WM_XBUTTONDOWN || wparam == WM_XBUTTONUP)) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        const WORD button = HIWORD(info->mouseData);
        const bool mirroring = g_api.is_running != nullptr && g_api.is_running() != 0;
        if ((button == XBUTTON1 || button == XBUTTON2) && mirroring) {
            if (wparam == WM_XBUTTONDOWN) {
                // 0 = 后退键（上一档），1 = 前进键（下一档）
                const WPARAM step = button == XBUTTON1 ? 0 : 1;
                PostMessageW(g_hwnd.load(), kWmCycleMode, step, 0);
            }
            // 按下与抬起都吞掉，避免目标程序收到孤立的抬起事件
            return 1;
        }
    }
    return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

void MouseHookThreadMain() {
    g_hook_thread_id.store(GetCurrentThreadId());
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, ModuleHandle(), 0);
    if (g_mouse_hook == nullptr) {
        Log("[webview] SetWindowsHookEx(WH_MOUSE_LL) 失败：%lu", GetLastError());
        g_hook_thread_id.store(0);
        return;
    }
    g_hook_ready.store(true);
    Log("[webview] mouse side buttons hooked");

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
    g_hook_ready.store(false);
    g_hook_thread_id.store(0);
}

bool InstallMouseHook() {
    if (g_hook_thread.joinable()) return true;
    g_hook_ready.store(false);
    g_hook_thread = std::thread(MouseHookThreadMain);
    // 等钩子挂上再返回，日志里能确定安装结果
    for (int attempt = 0; attempt < 50 && !g_hook_ready.load(); ++attempt) Sleep(2);
    return g_hook_ready.load();
}

void UninstallMouseHook() {
    if (!g_hook_thread.joinable()) return;
    const DWORD thread_id = g_hook_thread_id.load();
    if (thread_id != 0) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    g_hook_thread.join();
}

// ---------------------------------------------------------------------------
// JSON 桥
// ---------------------------------------------------------------------------

struct BridgeError {
    int32_t code;
    std::string message;
};

std::vector<klive_monitor_desc> EnumerateMonitors() {
    constexpr int kCapacity = 16;
    std::vector<klive_monitor_desc> items(kCapacity);
    const int count = g_api.enumerate_monitors(items.data(), kCapacity);
    items.resize(static_cast<size_t>(std::max(count, 0)));
    return items;
}

json BuildMonitors() {
    const std::vector<klive_monitor_desc> items = EnumerateMonitors();
    json array = json::array();
    for (const klive_monitor_desc& item : items) {
        array.push_back(json{
            {"index", item.index},
            {"name", FixedToUtf8(item.name, sizeof(item.name))},
            {"friendlyName", FixedToUtf8(item.friendly_name, sizeof(item.friendly_name))},
            {"adapterName", FixedToUtf8(item.adapter_name, sizeof(item.adapter_name))},
            {"x", item.x},
            {"y", item.y},
            {"width", item.width},
            {"height", item.height},
            {"isPrimary", item.is_primary != 0},
            {"adapterLuidLow", item.adapter_luid_low},
            {"adapterLuidHigh", item.adapter_luid_high},
            {"refreshHz", item.refresh_hz},
            {"hdr", item.hdr != 0},
        });
    }
    return array;
}

json BuildWindows() {
    constexpr int kCapacity = 128;
    std::vector<klive_window_desc> items(kCapacity);
    int count = g_api.enumerate_windows(items.data(), kCapacity);
    json array = json::array();
    for (int i = 0; i < count; ++i) {
        const klive_window_desc& item = items[static_cast<size_t>(i)];
        array.push_back(json{
            {"handle", std::to_string(item.handle)},
            {"processId", item.process_id},
            {"title", FixedToUtf8(item.title, sizeof(item.title))},
            {"className", FixedToUtf8(item.class_name, sizeof(item.class_name))},
            {"processName", FixedToUtf8(item.process_name, sizeof(item.process_name))},
            {"width", item.width},
            {"height", item.height},
        });
    }
    return array;
}

// ---------------------------------------------------------------------------
// 镜像设置（档位与全屏档的显示器选择）
//
// 屏幕按 DXGI 设备名 + 型号持久化，不按 index：热插拔或改显示配置后 index 会漂移。
// 只在这里读写，调用点都在 UI 线程（桥消息、侧键、启动参数）。
// ---------------------------------------------------------------------------

struct MirrorSettings {
    int32_t mode = KLIVE_MODE_FULLSCREEN;
    std::string source_monitor;
    std::string source_friendly;
    std::string target_monitor;
    std::string target_friendly;
    /* 色调曲线控制点的纵坐标（横坐标固定在 TONE_CURVE_KNOTS），只负责来回传递 */
    std::vector<float> tone_curve;
    int32_t source_index = -1;
    int32_t target_index = -1;
};

/*
 * 默认曲线控制点：0 → 0.35 → 0.7 直通，0.9 起 shoulder，1.2 / 1.5 压到 SDR 白附近。
 * 与 core 的 ToneCurve.Default 保持一致；面板启动后会用完整 LUT 覆盖。
 */
std::vector<float> DefaultToneCurve() { return {0.0f, 0.35f, 0.7f, 0.9f, 0.975f, 0.9857f}; }

MirrorSettings g_settings;

void ResolveSettingsMonitors() {
    const std::vector<klive_monitor_desc> monitors = EnumerateMonitors();
    auto find = [&monitors](const std::string& name, const std::string& friendly) -> int32_t {
        if (!name.empty()) {
            for (const klive_monitor_desc& item : monitors) {
                if (name == FixedToUtf8(item.name, sizeof(item.name))) return item.index;
            }
        }
        if (!friendly.empty()) {
            for (const klive_monitor_desc& item : monitors) {
                if (friendly == FixedToUtf8(item.friendly_name, sizeof(item.friendly_name))) {
                    return item.index;
                }
            }
        }
        return -1;
    };
    g_settings.source_index = find(g_settings.source_monitor, g_settings.source_friendly);
    g_settings.target_index = find(g_settings.target_monitor, g_settings.target_friendly);
}

void ApplySettingsJson(const char* text) {
    g_settings = MirrorSettings{};
    Log("[webview] settings json: %s", text != nullptr ? text : "");
    if (text != nullptr && *text != '\0') {
        try {
            const json value = json::parse(text);
            g_settings.mode = value.value("mode", 0);
            g_settings.source_monitor = value.value("sourceMonitor", std::string());
            g_settings.source_friendly = value.value("sourceFriendly", std::string());
            g_settings.target_monitor = value.value("targetMonitor", std::string());
            g_settings.target_friendly = value.value("targetFriendly", std::string());
            if (value.contains("toneCurve") && value["toneCurve"].is_array()) {
                g_settings.tone_curve.clear();
                for (const json& item : value["toneCurve"]) {
                    if (item.is_number()) {
                        g_settings.tone_curve.push_back(
                            std::clamp(item.get<float>(), 0.0f, 1.0f));
                    }
                }
            }
        } catch (const std::exception& error) {
            Log("[webview] settings json 无法解析：%s", error.what());
        }
    }
    if (g_settings.mode < 0 || g_settings.mode >= KLIVE_MODE_COUNT) g_settings.mode = 0;
    if (g_settings.tone_curve.empty()) g_settings.tone_curve = DefaultToneCurve();
    ResolveSettingsMonitors();
    Log("[webview] settings: mode=%d source=%d target=%d curvePoints=%zu", g_settings.mode,
        g_settings.source_index, g_settings.target_index, g_settings.tone_curve.size());
}

/* source_index < 0 表示保留原来的源（例如用窗口做全屏镜像时）。 */
void UpdateSettings(int32_t mode, int32_t source_index, int32_t target_index) {
    const std::vector<klive_monitor_desc> monitors = EnumerateMonitors();
    auto assign = [&monitors](int32_t index, std::string& name, std::string& friendly) {
        if (index < 0) return;
        for (const klive_monitor_desc& item : monitors) {
            if (item.index != index) continue;
            name = FixedToUtf8(item.name, sizeof(item.name));
            friendly = FixedToUtf8(item.friendly_name, sizeof(item.friendly_name));
            return;
        }
    };
    g_settings.mode = mode;
    assign(source_index, g_settings.source_monitor, g_settings.source_friendly);
    assign(target_index, g_settings.target_monitor, g_settings.target_friendly);
    ResolveSettingsMonitors();
}

std::string SettingsJson() {
    return json{
        {"mode", g_settings.mode},
        {"sourceMonitor", g_settings.source_monitor},
        {"sourceFriendly", g_settings.source_friendly},
        {"targetMonitor", g_settings.target_monitor},
        {"targetFriendly", g_settings.target_friendly},
        {"toneCurve", g_settings.tone_curve},
    }
        .dump();
}

void SaveSettings() {
    // 落盘由插件写回 keel 配置；这里只把最新 JSON 交出去。
    if (g_ui != nullptr && g_ui->save_settings != nullptr) {
        g_ui->save_settings(SettingsJson().c_str(), g_ui->save_settings_user);
    }
}

json BuildSettingsInfo() {
    ResolveSettingsMonitors();
    return json{
        {"mode", g_settings.mode},
        {"sourceIndex", g_settings.source_index},
        {"targetIndex", g_settings.target_index},
        {"toneCurve", g_settings.tone_curve},
    };
}

json BuildStats() {
    klive_mirror_stats stats{};
    g_api.get_stats(&stats);
    return json{
        {"running", stats.running != 0},
        {"frames", stats.frames},
        {"fps", stats.fps},
        {"frameWidth", stats.frame_width},
        {"frameHeight", stats.frame_height},
        {"lastError", stats.last_error},
        {"mode", stats.mode},
        {"sourceIndex", stats.source_index},
    };
}

int64_t ParseHandle(const json& value) {
    if (value.is_string()) {
        return std::stoll(value.get<std::string>());
    }
    if (value.is_number_unsigned()) return static_cast<int64_t>(value.get<uint64_t>());
    if (value.is_number_integer()) return value.get<int64_t>();
    return 0;
}

json Dispatch(const std::string& method, const json& params) {
    if (method == "enumerateMonitors") {
        return BuildMonitors();
    }
    if (method == "enumerateWindows") {
        return BuildWindows();
    }
    if (method == "getStats") {
        return BuildStats();
    }
    if (method == "getSettings") {
        return BuildSettingsInfo();
    }
    if (method == "setToneCurve") {
        // 拖曲线时只改运行参数，不落盘；关闭弹窗后由面板调 saveSettings 持久化
        if (!params.contains("lut") || !params["lut"].is_array()) {
            throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, "色调曲线缺少 lut 数组"};
        }
        std::vector<float> lut;
        lut.reserve(params["lut"].size());
        for (const json& item : params["lut"]) {
            if (!item.is_number()) {
                throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, "色调曲线只能包含数字"};
            }
            lut.push_back(item.get<float>());
        }
        if (lut.size() != KLIVE_TONE_CURVE_SIZE) {
            throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG,
                              "色调曲线必须包含 " + std::to_string(KLIVE_TONE_CURVE_SIZE) +
                                  " 个采样点"};
        }
        if (g_api.set_tone_curve != nullptr && g_api.set_tone_curve(lut.data(),
                                                                    static_cast<int32_t>(lut.size())) != 0) {
            throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, CaptureMessage()};
        }
        Log("[webview] tone curve updated (%zu points)", lut.size());
        return nullptr;
    }
    if (method == "saveSettings") {
        const int32_t mode = params.value("mode", g_settings.mode);
        if (mode < 0 || mode >= KLIVE_MODE_COUNT) {
            throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, "镜像档位无效"};
        }
        const int32_t source_index = params.value("sourceIndex", g_settings.source_index);
        const int32_t target_index = params.value("targetIndex", g_settings.target_index);
        UpdateSettings(mode, source_index, target_index);
        if (params.contains("toneCurve") && params["toneCurve"].is_array()) {
            std::vector<float> curve;
            for (const json& item : params["toneCurve"]) {
                if (item.is_number()) curve.push_back(std::clamp(item.get<float>(), 0.0f, 1.0f));
            }
            if (!curve.empty()) g_settings.tone_curve = std::move(curve);
        }
        // 运行中改档位立即生效；未运行时只是记下下次启动的默认值
        g_api.set_mode(mode);
        SaveSettings();
        Log("[webview] settings saved: mode=%d source=%d target=%d curvePoints=%zu", mode,
            source_index, target_index, g_settings.tone_curve.size());
        return nullptr;
    }
    if (method == "lastMessage") {
        return CaptureMessage();
    }
    if (method == "openConsole") {
        if (g_ui == nullptr || g_ui->open_console == nullptr) {
            throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, "当前宿主不支持打开终端"};
        }
        g_ui->open_console(g_ui->open_console_user);
        Log("[webview] console requested");
        return nullptr;
    }
    if (method == "stopMirror") {
        int32_t code = g_api.stop_mirror();
        if (code != 0) throw BridgeError{code, CaptureMessage()};
        return nullptr;
    }
    if (method == "startMirror") {
        klive_mirror_config config{};
        if (params.contains("windowHandle")) {
            config.window_handle = ParseHandle(params["windowHandle"]);
        }
        config.source_index = params.value("sourceIndex", 0);
        config.target_index = params.value("targetIndex", 0);
        config.scale_mode = params.value("scaleMode", 0);
        config.vsync = params.value("vsync", true) ? 1 : 0;
        config.capture_backend = params.value("captureBackend", 1);
        config.source_kind = params.value("sourceKind", 0);
        config.mode = params.value("mode", g_settings.mode);
        if (config.source_kind == KLIVE_SOURCE_WINDOW) {
            // 窗口采集只能走 WGC
            config.capture_backend = KLIVE_CAPTURE_WGC;
        }
        int32_t code = g_api.start_mirror(&config);
        if (code != 0) throw BridgeError{code, CaptureMessage()};
        // 全屏档记住了显示器；窗口源没有稳定身份，保留原来的显示器选择
        UpdateSettings(
            config.mode, config.source_kind == KLIVE_SOURCE_MONITOR ? config.source_index : -1,
            config.target_index);
        SaveSettings();
        Log("[webview] mirror started: source=%d target=%d kind=%d mode=%d", config.source_index,
            config.target_index, config.source_kind, config.mode);
        return nullptr;
    }
    throw BridgeError{KLIVE_WEBVIEW_E_INVALID_ARG, "未知方法：" + method};
}

std::string HandleBridgeMessage(const std::string& text) {
    json response;
    json request;
    try {
        request = json::parse(text);
    } catch (const std::exception& error) {
        response = json{
            {"id", 0},
            {"ok", false},
            {"error", {{"code", KLIVE_WEBVIEW_E_INVALID_ARG},
                       {"message", std::string("请求不是合法 JSON：") + error.what()}}},
        };
        return response.dump();
    }

    json id = request.contains("id") ? request["id"] : json(0);
    std::string method = request.value("method", "");
    json params = request.contains("params") ? request["params"] : json::object();

    response["id"] = id;
    try {
        response["ok"] = true;
        response["result"] = Dispatch(method, params);
    } catch (const BridgeError& error) {
        response["ok"] = false;
        response["error"] = json{{"code", error.code}, {"message", error.message}};
    } catch (const std::exception& error) {
        response["ok"] = false;
        response["error"] = json{
            {"code", KLIVE_WEBVIEW_E_INVALID_ARG},
            {"message", std::string("处理请求失败：") + error.what()},
        };
    }
    return response.dump();
}

// ---------------------------------------------------------------------------
// 页面资源
// ---------------------------------------------------------------------------

std::wstring UrlPathToRelative(const std::wstring& uri) {
    const std::wstring prefix = L"https://klive.local";
    if (uri.compare(0, prefix.size(), prefix) != 0) return std::wstring();
    std::wstring path = uri.substr(prefix.size());
    size_t query = path.find_first_of(L"?#");
    if (query != std::wstring::npos) path = path.substr(0, query);
    if (path.empty() || path == L"/") return L"index.html";
    if (path.front() == L'/') path.erase(path.begin());

    // 百分号解码（文件名里可能有空格 / 中文）
    std::wstring decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == L'%' && i + 2 < path.size()) {
            auto hex = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                return -1;
            };
            int high = hex(path[i + 1]);
            int low = hex(path[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<wchar_t>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(path[i]);
    }

    // 拒绝越界路径
    std::wstring lowered = decoded;
    for (wchar_t& c : lowered) {
        if (c == L'/') c = L'\\';
    }
    if (lowered.find(L"..") != std::wstring::npos) return std::wstring();
    return lowered;
}

const wchar_t* ContentTypeFor(const std::wstring& path) {
    auto ends_with = [&path](const wchar_t* suffix) {
        size_t length = wcslen(suffix);
        return path.size() >= length &&
               _wcsicmp(path.c_str() + path.size() - length, suffix) == 0;
    };
    if (ends_with(L".html")) return L"text/html; charset=utf-8";
    if (ends_with(L".js") || ends_with(L".mjs")) return L"text/javascript; charset=utf-8";
    if (ends_with(L".wasm")) return L"application/wasm";
    if (ends_with(L".css")) return L"text/css; charset=utf-8";
    if (ends_with(L".json")) return L"application/json; charset=utf-8";
    if (ends_with(L".ttf")) return L"font/ttf";
    if (ends_with(L".ttc")) return L"font/collection";
    if (ends_with(L".otf")) return L"font/otf";
    if (ends_with(L".svg")) return L"image/svg+xml";
    if (ends_with(L".png")) return L"image/png";
    if (ends_with(L".ico")) return L"image/x-icon";
    if (ends_with(L".woff2")) return L"font/woff2";
    return L"application/octet-stream";
}

std::wstring ResponseHeadersFor(const std::wstring& path) {
    std::wstring headers = L"Content-Type: ";
    headers += ContentTypeFor(path);
    headers +=
        L"\r\nCache-Control: no-store"
        L"\r\nCross-Origin-Opener-Policy: same-origin"
        L"\r\nCross-Origin-Embedder-Policy: require-corp";
    return headers;
}

/** 优先取现代中文字体；等线是单面 TTF，比微软雅黑的 TTC 更省心。 */
std::wstring SystemFontsDir() {
    static std::wstring cached = [] {
        wchar_t buffer[MAX_PATH] = {};
        UINT length = GetWindowsDirectoryW(buffer, MAX_PATH);
        return length > 0 ? JoinPath(std::wstring(buffer, length), L"Fonts")
                          : std::wstring(L"C:\\Windows\\Fonts");
    }();
    return cached;
}

std::wstring FindSystemFont() {
    static std::wstring cached = [] {
        std::wstring directory = SystemFontsDir();
        const wchar_t* candidates[] = {
            L"Deng.ttf",
            L"msyh.ttc",
            L"simhei.ttf",
            L"simsun.ttc",
        };
        for (const wchar_t* name : candidates) {
            std::wstring path = JoinPath(directory, name);
            DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                Log("[webview] 系统字体：%s", WideToUtf8(path).c_str());
                return path;
            }
        }
        return std::wstring();
    }();
    return cached;
}

ComPtr<IStream> MemoryStream(const std::string& text) {
    ComPtr<IStream> stream;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size());
    if (memory == nullptr) return stream;
    void* target = GlobalLock(memory);
    if (target != nullptr) {
        memcpy(target, text.data(), text.size());
        GlobalUnlock(memory);
    }
    CreateStreamOnHGlobal(memory, TRUE, stream.ReleaseAndGetAddressOf());
    return stream;
}

void ServeResource(
    UiContext* context, ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);

    LPWSTR raw_uri = nullptr;
    request->get_Uri(&raw_uri);
    std::wstring uri = raw_uri != nullptr ? raw_uri : L"";
    CoTaskMemFree(raw_uri);

    std::wstring relative = UrlPathToRelative(uri);
    if (relative.empty()) {
        ComPtr<IStream> stream = MemoryStream("forbidden");
        ComPtr<ICoreWebView2WebResourceResponse> response;
        context->environment->CreateWebResourceResponse(
            stream.Get(), 403, L"Forbidden", L"Content-Type: text/plain; charset=utf-8",
            response.ReleaseAndGetAddressOf());
        args->put_Response(response.Get());
        return;
    }

    // wasm 侧没有系统字体管理器，中文会渲染成豆腐块；这里把系统里的中文字体
    // 当成普通资源发出去，由 Compose 用 Font(identity, data) 载入。
    if (relative == L"system-font") {
        // 排障用：/system-font?font=simsun 换成宋体（带点阵字形，最容易看出缩放模糊）
        std::wstring font_path = uri.find(L"font=simsun") != std::wstring::npos
                                     ? JoinPath(SystemFontsDir(), L"simsun.ttc")
                                     : FindSystemFont();
        ComPtr<IStream> font_stream;
        if (font_path.empty() ||
            FAILED(SHCreateStreamOnFileEx(
                font_path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                FALSE, nullptr, font_stream.ReleaseAndGetAddressOf()))) {
            Log("[webview] 系统字体不可用");
            ComPtr<IStream> stream = MemoryStream("font not found");
            ComPtr<ICoreWebView2WebResourceResponse> response;
            context->environment->CreateWebResourceResponse(
                stream.Get(), 404, L"Not Found", L"Content-Type: text/plain; charset=utf-8",
                response.ReleaseAndGetAddressOf());
            args->put_Response(response.Get());
            return;
        }
        ComPtr<ICoreWebView2WebResourceResponse> response;
        context->environment->CreateWebResourceResponse(
            font_stream.Get(), 200, L"OK", ResponseHeadersFor(font_path).c_str(),
            response.ReleaseAndGetAddressOf());
        args->put_Response(response.Get());
        return;
    }

    std::wstring full_path = JoinPath(context->web_dir, relative);
    ComPtr<IStream> file_stream;
    HRESULT hr = SHCreateStreamOnFileEx(
        full_path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL, FALSE,
        nullptr, file_stream.ReleaseAndGetAddressOf());
    if (FAILED(hr) || file_stream == nullptr) {
        Log("[webview] 404 %s", WideToUtf8(relative).c_str());
        ComPtr<IStream> stream = MemoryStream("not found");
        ComPtr<ICoreWebView2WebResourceResponse> response;
        context->environment->CreateWebResourceResponse(
            stream.Get(), 404, L"Not Found", L"Content-Type: text/plain; charset=utf-8",
            response.ReleaseAndGetAddressOf());
        args->put_Response(response.Get());
        return;
    }

    ComPtr<ICoreWebView2WebResourceResponse> response;
    context->environment->CreateWebResourceResponse(
        file_stream.Get(), 200, L"OK", ResponseHeadersFor(relative).c_str(),
        response.ReleaseAndGetAddressOf());
    args->put_Response(response.Get());
}

// ---------------------------------------------------------------------------
// WebView2 配置
// ---------------------------------------------------------------------------

void ResizeWebView(UiContext* context, HWND hwnd) {
    if (context == nullptr || context->controller == nullptr) return;
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi > 0) {
        // 显式给出栅格化缩放，避免 WebView2 在 per-monitor DPI 下按 96 渲染再被拉伸
        ComPtr<ICoreWebView2Controller3> controller3;
        if (SUCCEEDED(context->controller.As(&controller3)) && controller3 != nullptr) {
            controller3->put_RasterizationScale(static_cast<double>(dpi) / 96.0);
        }
    }
    context->controller->put_Bounds(bounds);
}

void ConfigureWebView(UiContext* context) {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(context->webview->get_Settings(settings.ReleaseAndGetAddressOf())) &&
        settings != nullptr) {
        settings->put_AreDevToolsEnabled(context->devtools ? TRUE : FALSE);
        settings->put_AreDefaultContextMenusEnabled(context->devtools ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    ResizeWebView(context, g_hwnd.load());
    context->controller->put_IsVisible(TRUE);

    context->webview->AddWebResourceRequestedFilter(
        L"https://klive.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken resource_token{};
    context->webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [context](ICoreWebView2* /*sender*/,
                      ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ServeResource(context, args);
                return S_OK;
            })
            .Get(),
        &resource_token);

    EventRegistrationToken message_token{};
    context->webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                std::string text;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw != nullptr) {
                    text = WideToUtf8(raw);
                    CoTaskMemFree(raw);
                } else {
                    raw = nullptr;
                    if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw != nullptr) {
                        text = WideToUtf8(raw);
                        CoTaskMemFree(raw);
                    }
                }
                if (text.empty()) return S_OK;
                Log("[bridge] <- %s", text.c_str());
                std::string reply = HandleBridgeMessage(text);
                Log("[bridge] -> %s", reply.c_str());
                std::wstring wide = Utf8ToWide(reply.c_str());
                sender->PostWebMessageAsJson(wide.c_str());
                return S_OK;
            })
            .Get(),
        &message_token);

    EventRegistrationToken navigation_token{};
    context->webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2* /*sender*/,
               ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                args->get_Uri(&raw);
                std::wstring uri = raw != nullptr ? raw : L"";
                CoTaskMemFree(raw);
                const std::wstring prefix = L"https://klive.local/";
                if (uri != L"about:blank" && uri.compare(0, prefix.size(), prefix) != 0) {
                    Log("[webview] 拦截外部导航：%s", WideToUtf8(uri).c_str());
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            })
            .Get(),
        &navigation_token);

    context->webview->Navigate(L"https://klive.local/index.html");
}

void CreateWebView(UiContext* context) {
    const std::wstring profile_dir = JoinPath(context->data_dir, L"WebView2");
    SHCreateDirectoryExW(nullptr, profile_dir.c_str(), nullptr);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, profile_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [context](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    SetError("WebView2 Runtime 初始化失败（HRESULT=" +
                             std::to_string(static_cast<long>(result)) +
                             "），请安装 Microsoft Edge WebView2 Runtime");
                    FinishInit(context, KLIVE_WEBVIEW_E_RUNTIME);
                    return S_OK;
                }
                context->environment = environment;

                HRESULT controller_result = environment->CreateCoreWebView2Controller(
                    g_hwnd.load(),
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [context](HRESULT create_result,
                                  ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(create_result) || controller == nullptr) {
                                SetError("WebView2 控制器创建失败（HRESULT=" +
                                         std::to_string(static_cast<long>(create_result)) + "）");
                                FinishInit(context, KLIVE_WEBVIEW_E_INIT);
                                return S_OK;
                            }
                            context->controller = controller;
                            controller->get_CoreWebView2(
                                context->webview.ReleaseAndGetAddressOf());
                            ConfigureWebView(context);

                            ShowWindow(g_hwnd.load(), SW_SHOW);
                            UpdateWindow(g_hwnd.load());
                            Log("[webview] ready");
                            FinishInit(context, KLIVE_WEBVIEW_OK);
                            return S_OK;
                        })
                        .Get());
                if (FAILED(controller_result)) {
                    SetError("WebView2 控制器请求失败（HRESULT=" +
                             std::to_string(static_cast<long>(controller_result)) + "）");
                    FinishInit(context, KLIVE_WEBVIEW_E_INIT);
                }
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        SetError("WebView2 环境创建失败（HRESULT=" + std::to_string(static_cast<long>(hr)) +
                 "），请安装 Microsoft Edge WebView2 Runtime");
        FinishInit(context, KLIVE_WEBVIEW_E_RUNTIME);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            ResizeWebView(g_ui, hwnd);
            return 0;
        case WM_DPICHANGED: {
            // 跨屏拖动时按系统建议的矩形重排，并让 WebView2 重新取缩放
            auto* suggested = reinterpret_cast<RECT*>(lparam);
            if (suggested != nullptr) {
                SetWindowPos(
                    hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ResizeWebView(g_ui, hwnd);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            if (g_ui != nullptr) {
                info->ptMinTrackSize.x = g_ui->min_width;
                info->ptMinTrackSize.y = g_ui->min_height;
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case kWmShowPanel:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case kWmCycleMode: {
            const int32_t current = g_api.get_mode();
            const int32_t delta = wparam == 0 ? -1 : 1;
            const int32_t next = (current + delta + KLIVE_MODE_COUNT) % KLIVE_MODE_COUNT;
            if (g_api.set_mode(next) != 0) return 0;
            g_settings.mode = next;
            SaveSettings();
            // 切档提示由 klive_capture 的镜像渲染循环画到画面左上角，这里不再弹系统气泡
            Log("[webview] mode -> %d", next);
            return 0;
        }
        case kWmStopAndDestroy:
            StopMirror();
            RemoveTrayIcon();
            DestroyWindow(hwnd);
            return 0;
        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case kMenuShow:
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    return 0;
                case kMenuStop:
                    StopMirror();
                    return 0;
                case kMenuExit:
                    StopMirror();
                    g_exit_process.store(true);
                    RemoveTrayIcon();
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            return 0;
        }
        case kWmTray: {
            UINT event = LOWORD(lparam);
            if (event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;
        }
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam != 0) StopMirror();
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }

    if (g_taskbar_created_message != 0 && message == g_taskbar_created_message) {
        AddTrayIcon(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool RegisterWindowClass() {
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = ModuleHandle();
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = kWindowClass;
        registered = RegisterClassExW(&window_class) != 0;
        if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            // 同一进程里旧副本退出时没来得及注销（异常路径）；类已存在就继续用
            registered = true;
        }
    });
    return registered;
}

void UiThreadMain(UiContext* context) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 线程级 DPI 感知优先于进程级：宿主可能已经被别的模块设成 system aware
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 配置里的尺寸按 96 DPI 的逻辑像素理解；线程已经是 per-monitor aware，
    // 这里再按主屏缩放换算成物理像素，否则高 DPI 下窗口会偏小、字也会被拉伸。
    UINT dpi = GetDpiForSystem();
    double dpi_scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
    auto scale_size = [dpi_scale](int value) {
        return static_cast<int>(value * dpi_scale + 0.5);
    };
    context->width = scale_size(context->width);
    context->height = scale_size(context->height);
    context->min_width = scale_size(context->min_width);
    context->min_height = scale_size(context->min_height);
    Log(
        "[webview] dpi=%u scale=%.2f window=%dx%d", dpi, dpi_scale, context->width,
        context->height);

    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");

    g_icon = LoadIconW(ModuleHandle(), MAKEINTRESOURCEW(kIconResourceId));
    if (g_icon == nullptr) {
        g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    if (!RegisterWindowClass()) {
        SetError("注册窗口类失败（GetLastError=" + std::to_string(GetLastError()) + "）");
        FinishInit(context, KLIVE_WEBVIEW_E_WINDOW);
        delete context;
        CoUninitialize();
        g_running.store(false);
        return;
    }

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = context->width > 0 ? context->width : 1120;
    int height = context->height > 0 ? context->height : 760;
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + ((work.bottom - work.top) - height) / 2;

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
        nullptr, ModuleHandle(), nullptr);
    if (hwnd == nullptr) {
        SetError("创建窗口失败（GetLastError=" + std::to_string(GetLastError()) + "）");
        FinishInit(context, KLIVE_WEBVIEW_E_WINDOW);
        delete context;
        CoUninitialize();
        g_running.store(false);
        return;
    }

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));

    // 标题栏跟随暗色面板：由 DWM 画白字，避免黑字压在深色背景上看不清
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
    }

    g_hwnd.store(hwnd);
    g_ui = context;
    Log("[webview] window dpi=%u size=%dx%d", GetDpiForWindow(hwnd), width, height);
    AddTrayIcon(hwnd);
    // 低级鼠标钩子必须挂在有消息循环的线程上，UI 线程正好满足
    InstallMouseHook();
    CreateWebView(context);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // 必须在本 DLL 卸载前摘钩子，否则回调指针会指向已卸载的代码
    UninstallMouseHook();

    // 插件重载时本 DLL 会被卸下再加载；窗口类不注销会把旧模块的 wndproc 留在进程里
    UnregisterClassW(kWindowClass, ModuleHandle());

    g_hwnd.store(nullptr);
    g_ui = nullptr;
    delete context;
    CoUninitialize();
    g_running.store(false);

    if (g_exit_process.load()) {
        // 宿主没有「请求停机」的 API；托盘退出直接结束进程，先给日志一点时间。
        Sleep(100);
        ExitProcess(0);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 导出函数
// ---------------------------------------------------------------------------

extern "C" {

int32_t klive_webview_start(const klive_webview_config* config) {
    if (config == nullptr || config->native_dir == nullptr || config->web_dir == nullptr) {
        SetError("参数无效：native_dir / web_dir 不能为空");
        return KLIVE_WEBVIEW_E_INVALID_ARG;
    }
    if (config->data_dir == nullptr || *config->data_dir == '\0') {
        SetError("参数无效：data_dir 不能为空");
        return KLIVE_WEBVIEW_E_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(g_thread_lock);
    if (g_running.load()) return KLIVE_WEBVIEW_E_ALREADY_RUNNING;

    SetError("");

    // DPI 感知是进程级的，必须在建窗口之前设置
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        // 宿主可能已经设过；UI 线程还会再做一次线程级覆盖
        Log("[webview] 进程 DPI 感知设置失败：%lu", GetLastError());
    }

    auto* context = new UiContext();
    context->native_dir = AbsolutePath(Utf8ToWide(config->native_dir));
    context->web_dir = AbsolutePath(Utf8ToWide(config->web_dir));
    context->data_dir = AbsolutePath(Utf8ToWide(config->data_dir));
    context->width = config->width > 0 ? config->width : 1120;
    context->height = config->height > 0 ? config->height : 760;
    context->min_width = config->min_width > 0 ? config->min_width : 920;
    context->min_height = config->min_height > 0 ? config->min_height : 640;
    context->devtools = config->enable_devtools != 0;
    context->open_console = config->open_console;
    context->open_console_user = config->open_console_user;
    context->settings_json =
        config->settings_json != nullptr ? std::string(config->settings_json) : std::string();
    context->save_settings = config->save_settings;
    context->save_settings_user = config->save_settings_user;

    if (!LoadCaptureApi(context->native_dir)) {
        delete context;
        return KLIVE_WEBVIEW_E_CAPTURE;
    }
    // 设置由插件从 keel 配置读出后传入，原生不再自己落盘
    ApplySettingsJson(context->settings_json.c_str());

    std::future<int32_t> init_result = context->init.get_future();
    g_exit_process.store(false);
    g_running.store(true);

    g_thread = std::thread(UiThreadMain, context);
    int32_t result = KLIVE_WEBVIEW_E_TIMEOUT;
    // keel 的 START 握手只有 5s，这里最多等 3s；超时视为成功，面板会在就绪后自己出现。
    if (init_result.wait_for(std::chrono::seconds(3)) == std::future_status::ready) {
        result = init_result.get();
    } else {
        Log("[webview] 初始化仍在进行，先返回成功");
        result = KLIVE_WEBVIEW_OK;
    }

    if (result != KLIVE_WEBVIEW_OK) {
        HWND hwnd = g_hwnd.load();
        if (hwnd != nullptr) {
            PostMessageW(hwnd, kWmStopAndDestroy, 0, 0);
        }
        if (g_thread.joinable()) g_thread.join();
        g_running.store(false);
        return result;
    }

    Log("[webview] started");
    return KLIVE_WEBVIEW_OK;
}

int32_t klive_webview_stop(void) {
    std::lock_guard<std::mutex> lock(g_thread_lock);
    if (!g_running.load()) return KLIVE_WEBVIEW_E_NOT_RUNNING;

    HWND hwnd = g_hwnd.load();
    if (hwnd != nullptr) {
        PostMessageW(hwnd, kWmStopAndDestroy, 0, 0);
    }
    if (g_thread.joinable()) {
        // WebView2 的关闭是同步的，这里等它把消息循环跑完
        g_thread.join();
    }
    g_running.store(false);
    Log("[webview] stopped");
    return KLIVE_WEBVIEW_OK;
}

int32_t klive_webview_is_running(void) { return g_running.load() ? 1 : 0; }

int32_t klive_webview_show(void) {
    HWND hwnd = g_hwnd.load();
    if (hwnd == nullptr) return KLIVE_WEBVIEW_E_NOT_RUNNING;
    PostMessageW(hwnd, kWmShowPanel, 0, 0);
    return KLIVE_WEBVIEW_OK;
}

const char* klive_webview_last_error(void) {
    static thread_local std::string buffer;
    buffer = LastErrorText();
    return buffer.c_str();
}

const char* klive_webview_probe(const char* native_dir) {
    static thread_local std::string buffer;
    if (native_dir == nullptr) {
        buffer = "{\"error\":\"native_dir 为空\"}";
        return buffer.c_str();
    }
    if (!LoadCaptureApi(Utf8ToWide(native_dir))) {
        buffer = json{{"error", LastErrorText()}}.dump();
        return buffer.c_str();
    }
    json result{{"monitors", BuildMonitors()}, {"windows", BuildWindows()}};
    buffer = result.dump(2);
    return buffer.c_str();
}

}  // extern "C"
