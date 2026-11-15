/*
 * klive 原生核心的 C ABI。
 *
 * 只暴露 C 结构体和函数，方便三种消费者复用同一份二进制：
 *   - JVM 外壳（JNA，见 src/jvmMain/kotlin/moe/lolosia/klive/capture）
 *   - 将来的 keel 插件（Kotlin/Native cinterop）
 *   - 其它 C/C++ 工具
 *
 * 采集支持两种后端：
 *   - 0 = DXGI Desktop Duplication：每台显示器同一时间只能有一个消费者，
 *     与 OBS 的「显示器采集」互斥；
 *   - 1 = Windows Graphics Capture（WGC，Win10 1903+）：可与其它程序同时采集，
 *     也是窗口采集的唯一后端。
 * 呈现统一走 D3D11 交换链，全程 GPU 内零拷贝（只有一次 CopyResource 解耦帧池）。
 *
 * 结构体里不放 4 字节以下的字段，且总大小保持 8 的整数倍，
 * 这样 JNA 的默认对齐与 MSVC x64 的默认对齐结果一致。
 */
#ifndef KLIVE_H
#define KLIVE_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(KLIVE_BUILD)
#    define KLIVE_API __declspec(dllexport)
#  else
#    define KLIVE_API
#  endif
#else
#  define KLIVE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KLIVE_OK = 0,
    KLIVE_E_INVALID_ARG = -1,
    KLIVE_E_ALREADY_RUNNING = -2,
    KLIVE_E_NOT_RUNNING = -3,
    KLIVE_E_NO_MONITOR = -4,
    KLIVE_E_DEVICE = -5,
    KLIVE_E_DUPLICATION = -6,
    KLIVE_E_WINDOW = -7,
    KLIVE_E_UNSUPPORTED = -8,
    KLIVE_E_CAPTURE = -9,
};

enum {
    KLIVE_CAPTURE_DUPLICATION = 0,
    KLIVE_CAPTURE_WGC = 1,
};

enum {
    KLIVE_SOURCE_MONITOR = 0,
    KLIVE_SOURCE_WINDOW = 1,
};

/*
 * 镜像档位。0 为固定采集源，1 起由鼠标 / 前台窗口动态决定源。
 * 缩放档位同样跟随鼠标所在显示器，并排除目标显示器。
 */
enum {
    KLIVE_MODE_FULLSCREEN = 0,
    KLIVE_MODE_POINTER_MONITOR = 1,
    KLIVE_MODE_POINTER_WINDOW = 2,
    KLIVE_MODE_FOCUS_WINDOW = 3,
    KLIVE_MODE_ZOOM_2X = 4,
    KLIVE_MODE_ZOOM_3X = 5,
    KLIVE_MODE_ZOOM_4X = 6,
    KLIVE_MODE_COUNT = 7,
};

/*
 * 一台显示器。
 * 坐标是物理像素（进程在 klive_initialize 里被设为 per-monitor DPI aware）。
 * 布局与 Kotlin 侧 KliveMonitorStruct 一一对应，改动时两边同步。
 */
typedef struct klive_monitor_desc {
    int32_t index;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t is_primary;
    int32_t adapter_luid_low;
    int32_t adapter_luid_high;
    float   refresh_hz;
    char    name[64];          /* DXGI 设备名，例如 "\\.\DISPLAY1" */
    char    friendly_name[96]; /* 显示器型号等，可能为空 */
    char    adapter_name[96];  /* 所在显卡名称 */
} klive_monitor_desc;

/*
 * 一个可采集的顶层窗口。
 * 布局与 Kotlin 侧 KliveWindowStruct 一一对应。
 */
typedef struct klive_window_desc {
    int64_t handle;         /* HWND，仅本进程内有效 */
    int32_t process_id;
    int32_t width;          /* 窗口客户区之外的整窗尺寸 */
    int32_t height;
    int32_t reserved;
    char    title[160];
    char    class_name[96];
    char    process_name[96]; /* 可执行文件名，例如 "game.exe" */
} klive_window_desc;

typedef struct klive_mirror_config {
    int64_t window_handle;   /* source_kind == KLIVE_SOURCE_WINDOW 时使用 */
    int32_t source_index;    /* 源显示器 index（klive_enumerate_monitors 的顺序） */
    int32_t target_index;    /* 目标显示器 index */
    int32_t scale_mode;      /* 0 = 等比填充（留黑边），1 = 拉伸铺满 */
    int32_t vsync;           /* 1 = Present 垂直同步 */
    int32_t capture_backend; /* KLIVE_CAPTURE_* */
    int32_t source_kind;     /* KLIVE_SOURCE_*，窗口采集只支持 WGC */
    int32_t mode;            /* KLIVE_MODE_*，非 0 档忽略 source_index / window_handle */
    int32_t reserved;        /* 补齐到 8 的整数倍 */
} klive_mirror_config;

typedef struct klive_mirror_stats {
    int64_t frames;
    double  fps;
    int32_t frame_width;
    int32_t frame_height;
    int32_t running;
    int32_t last_error;
    int32_t mode;            /* 当前档位 KLIVE_MODE_* */
    int32_t source_index;    /* 当前实际源显示器；窗口源或未解析时为 -1 */
} klive_mirror_stats;

/* 进程级初始化（DPI 感知）。可重复调用。 */
KLIVE_API void klive_initialize(void);

/* 枚举所有可采集的显示器，返回实际写入 out 的数量（最多 capacity 个）。 */
KLIVE_API int32_t klive_enumerate_monitors(klive_monitor_desc* out, int32_t capacity);

/* 枚举可采集的顶层窗口（已过滤掉隐藏 / 工具窗 / 本进程窗口）。 */
KLIVE_API int32_t klive_enumerate_windows(klive_window_desc* out, int32_t capacity);

/* 开始镜像，默认把目标显示器铺满（置顶无边框窗口）。 */
KLIVE_API int32_t klive_start_mirror(const klive_mirror_config* config);

/* 停止镜像并等待渲染线程退出。 */
KLIVE_API int32_t klive_stop_mirror(void);

KLIVE_API int32_t klive_is_running(void);

/*
 * 切换镜像档位；运行中立即生效，未运行时作为下一次启动的默认值。
 * 档位非法返回 KLIVE_E_INVALID_ARG。
 */
KLIVE_API int32_t klive_set_mode(int32_t mode);

KLIVE_API int32_t klive_get_mode(void);

KLIVE_API int32_t klive_get_stats(klive_mirror_stats* out);

/* 最近一次错误的可读描述（UTF-8），无错误时为空串。 */
KLIVE_API const char* klive_last_message(void);

/* 停止并释放进程级资源。 */
KLIVE_API void klive_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* KLIVE_H */
