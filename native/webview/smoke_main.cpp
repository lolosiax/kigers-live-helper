/*
 * `klive_webview` 的控制台自检：按绝对路径加载同目录的 klive_webview.dll，
 * 打开 smoke 页面，把桥日志打到 stdout。
 *
 * 用法：
 *   klive_webview_smoke.exe [web_dir] [--exit-after=ms]
 */

#include <windows.h>

#include <cstdio>
#include <string>

#include "klive_webview.h"

namespace {

typedef int32_t (*StartFn)(const klive_webview_config*);
typedef int32_t (*StopFn)();
typedef const char* (*LastErrorFn)();

std::wstring ExeDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer);
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length > 0 ? length - 1 : 0), '\0');
    if (length > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), length, nullptr, nullptr);
    }
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring exe_dir = ExeDirectory();
    std::wstring dll_path = exe_dir + L"\\klive_webview.dll";
    std::wstring web_dir = exe_dir + L"\\smoke";
    DWORD exit_after = 0;

    for (int i = 1; i < argc; ++i) {
        if (wcsncmp(argv[i], L"--exit-after=", 13) == 0) {
            exit_after = wcstoul(argv[i] + 13, nullptr, 10);
        } else if (argv[i][0] != L'-') {
            web_dir = argv[i];
        }
    }

    HMODULE module = LoadLibraryExW(
        dll_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        printf("load klive_webview.dll failed: %s (GetLastError=%lu)\n", ToUtf8(dll_path).c_str(),
               GetLastError());
        return 2;
    }

    auto start = reinterpret_cast<StartFn>(GetProcAddress(module, "klive_webview_start"));
    auto stop = reinterpret_cast<StopFn>(GetProcAddress(module, "klive_webview_stop"));
    auto last_error =
        reinterpret_cast<LastErrorFn>(GetProcAddress(module, "klive_webview_last_error"));
    if (start == nullptr || stop == nullptr || last_error == nullptr) {
        printf("klive_webview.dll missing exports\n");
        return 2;
    }

    // 让桥协议的回包打到 stdout，便于自动比对
    _wputenv_s(L"KLIVE_WEBVIEW_LOG", L"1");

    klive_webview_config config{};
    std::string native_dir = ToUtf8(exe_dir);
    std::string web_path = ToUtf8(web_dir);
    config.native_dir = native_dir.c_str();
    config.web_dir = web_path.c_str();
    config.user_data_dir = nullptr;
    config.width = 900;
    config.height = 640;
    config.min_width = 640;
    config.min_height = 480;
    config.enable_devtools = 1;

    int32_t code = start(&config);
    printf("klive_webview_start rc=%d\n", code);
    fflush(stdout);
    if (code != 0) {
        printf("error: %s\n", last_error());
        return 2;
    }

    if (exit_after > 0) {
        Sleep(exit_after);
    } else {
        printf("press Enter to stop...\n");
        fflush(stdout);
        getchar();
    }

    printf("klive_webview_stop rc=%d\n", stop());
    fflush(stdout);
    return 0;
}
