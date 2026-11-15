#include "internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>

void klive_initialize(void) {
    static std::once_flag once;
    std::call_once(once, [] {
        // 让 DXGI 给出的显示器坐标就是物理像素，Win32 窗口定位才能对得上
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    });
}

int32_t klive_enumerate_monitors(klive_monitor_desc* out, int32_t capacity) {
    std::vector<KliveOutput> outputs = klive_collect_outputs();
    if (out == nullptr || capacity <= 0) return 0;

    const int32_t count = std::min(static_cast<int32_t>(outputs.size()), capacity);
    for (int32_t i = 0; i < count; ++i) {
        const KliveOutput& entry = outputs[static_cast<size_t>(i)];
        klive_monitor_desc& desc = out[i];

        MONITORINFO info{};
        info.cbSize = sizeof(info);
        const BOOL has_info = GetMonitorInfoW(entry.desc.Monitor, &info);
        const RECT& rect = has_info ? info.rcMonitor : entry.desc.DesktopCoordinates;

        desc = klive_monitor_desc{};
        desc.index = entry.index;
        desc.x = rect.left;
        desc.y = rect.top;
        desc.width = rect.right - rect.left;
        desc.height = rect.bottom - rect.top;
        desc.is_primary = (has_info && (info.dwFlags & MONITORINFOF_PRIMARY)) ? 1 : 0;
        desc.adapter_luid_low = entry.luid.LowPart;
        desc.adapter_luid_high = entry.luid.HighPart;
        desc.refresh_hz = entry.refresh_hz;
        memcpy(desc.name, entry.name, sizeof(desc.name));
        memcpy(desc.friendly_name, entry.friendly_name, sizeof(desc.friendly_name));
        memcpy(desc.adapter_name, entry.adapter_name, sizeof(desc.adapter_name));
    }
    return count;
}

int32_t klive_enumerate_windows(klive_window_desc* out, int32_t capacity) {
    if (out == nullptr || capacity <= 0) return 0;

    std::vector<klive_window_desc> windows = klive_collect_windows();
    const int32_t count = std::min(static_cast<int32_t>(windows.size()), capacity);
    for (int32_t i = 0; i < count; ++i) {
        out[i] = windows[static_cast<size_t>(i)];
    }
    return count;
}

int32_t klive_start_mirror(const klive_mirror_config* config) {
    if (config == nullptr) {
        klive_set_message("镜像参数为空");
        return KLIVE_E_INVALID_ARG;
    }
    return klive_mirror_start(*config);
}

int32_t klive_stop_mirror(void) {
    return klive_mirror_stop();
}

int32_t klive_is_running(void) {
    return klive_session().running.load() ? 1 : 0;
}

int32_t klive_set_mode(int32_t mode) {
    if (mode < 0 || mode >= KLIVE_MODE_COUNT) {
        klive_set_message("镜像档位无效");
        return KLIVE_E_INVALID_ARG;
    }
    klive_session().mode = mode;
    return KLIVE_OK;
}

int32_t klive_get_mode(void) {
    return klive_session().mode.load();
}

int32_t klive_get_stats(klive_mirror_stats* out) {
    if (out == nullptr) return KLIVE_E_INVALID_ARG;
    KliveMirrorSession& session = klive_session();
    out->frames = session.frames.load();
    out->fps = session.fps.load();
    out->frame_width = session.frame_width.load();
    out->frame_height = session.frame_height.load();
    out->running = session.running.load() ? 1 : 0;
    out->last_error = session.last_error.load();
    out->mode = session.mode.load();
    out->source_index = session.source_index.load();
    return KLIVE_OK;
}

const char* klive_last_message(void) {
    return klive_message();
}

void klive_shutdown(void) {
    klive_mirror_stop();
}
