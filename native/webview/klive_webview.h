/*
 * WebView2 外壳的 C ABI。
 *
 * 这一层只做三件事：Win32 窗口 + WebView2 + JSON 桥；采集全部转发给
 * `klive_capture.dll`（见 native/include/klive.h）。插件侧只按绝对路径
 * 加载本 DLL，不静态链接任何 C++ 类型。
 */
#ifndef KLIVE_WEBVIEW_H
#define KLIVE_WEBVIEW_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(KLIVE_WEBVIEW_BUILD)
#    define KLIVE_WEBVIEW_API __declspec(dllexport)
#  else
#    define KLIVE_WEBVIEW_API
#  endif
#else
#  define KLIVE_WEBVIEW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KLIVE_WEBVIEW_OK 0
#define KLIVE_WEBVIEW_E_INVALID_ARG (-1)
#define KLIVE_WEBVIEW_E_CAPTURE (-2)
#define KLIVE_WEBVIEW_E_RUNTIME (-3)
#define KLIVE_WEBVIEW_E_INIT (-4)
#define KLIVE_WEBVIEW_E_WINDOW (-5)
#define KLIVE_WEBVIEW_E_ALREADY_RUNNING (-6)
#define KLIVE_WEBVIEW_E_NOT_RUNNING (-7)
#define KLIVE_WEBVIEW_E_TIMEOUT (-8)

typedef struct klive_webview_config {
    const char* native_dir;     /* UTF-8：含 klive_capture.dll / klive_webview.dll / WebView2Loader.dll */
    const char* web_dir;        /* UTF-8：含 index.html 的目录 */
    const char* user_data_dir;  /* UTF-8，可空；空则用 %LOCALAPPDATA%\klive\WebView2 */
    int32_t width;
    int32_t height;
    int32_t min_width;
    int32_t min_height;
    int32_t enable_devtools;
    int32_t reserved;
} klive_webview_config;

/*
 * 启动外壳：内部起独立 UI 线程后返回；返回 KLIVE_WEBVIEW_OK 表示窗口与
 * WebView2 已就绪，失败时用 klive_webview_last_error() 取可展示文案。
 */
KLIVE_WEBVIEW_API int32_t klive_webview_start(const klive_webview_config* config);

/* 停止并等待 UI 线程退出（不结束进程）。 */
KLIVE_WEBVIEW_API int32_t klive_webview_stop(void);

KLIVE_WEBVIEW_API int32_t klive_webview_is_running(void);

/* 把被隐藏到托盘的面板重新显示出来。 */
KLIVE_WEBVIEW_API int32_t klive_webview_show(void);

/* 最近一次错误的 UTF-8 描述，没有错误时为空串。 */
KLIVE_WEBVIEW_API const char* klive_webview_last_error(void);

/*
 * 控制台自检：从 native_dir 加载采集 DLL，返回显示器与窗口的 JSON。
 * 返回的指针在下次调用前有效，仅供排障使用。
 */
KLIVE_WEBVIEW_API const char* klive_webview_probe(const char* native_dir);

#ifdef __cplusplus
}
#endif

#endif /* KLIVE_WEBVIEW_H */
