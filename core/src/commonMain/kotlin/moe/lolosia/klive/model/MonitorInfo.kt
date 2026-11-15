package moe.lolosia.klive.model

import kotlin.math.roundToInt

/**
 * 一台显示器（纯 Kotlin，commonMain）。
 *
 * 由原生层 klive_monitor_desc 转换而来，坐标单位是物理像素。
 */
data class MonitorInfo(
    val index: Int,
    val name: String,
    val friendlyName: String,
    val adapterName: String,
    val x: Int,
    val y: Int,
    val width: Int,
    val height: Int,
    val isPrimary: Boolean,
    val adapterLuidLow: Int,
    val adapterLuidHigh: Int,
    val refreshHz: Float,
) {
    /** 显卡标识：跨显卡的两台显示器无法直接用 D3D11 交换链呈现。 */
    val adapterKey: Long
        get() = (adapterLuidHigh.toLong() shl 32) or (adapterLuidLow.toLong() and 0xFFFF_FFFFL)

    val displayName: String
        get() = friendlyName.ifBlank { name.substringAfterLast('\\') }

    val resolutionLabel: String
        get() = "$width × $height"

    val refreshLabel: String
        get() = if (refreshHz >= 20f) "${refreshHz.roundToInt()} Hz" else "未知刷新率"

    val positionLabel: String
        get() = "($x, $y)"
}

enum class ScaleMode(val code: Int, val label: String) {
    Fit(0, "等比填充"),
    Stretch(1, "拉伸铺满"),
}

/**
 * 采集后端，与原生层 klive.h 的 KLIVE_CAPTURE_* 对应。
 *
 * 桌面复制对每台显示器是独占的（和 OBS 的「显示器采集」冲突），
 * WGC 则可以多个程序同时采集，也是窗口采集的唯一选择。
 */
enum class CaptureBackend(val code: Int, val label: String, val description: String) {
    DesktopDuplication(0, "桌面复制", "DXGI Desktop Duplication · 独占，与 OBS 等采集冲突"),
    WindowsGraphicsCapture(1, "WGC", "Windows Graphics Capture · 可多程序同时采集"),
}

/** 采集源类型，与原生层 KLIVE_SOURCE_* 对应。 */
enum class CaptureSourceKind(val code: Int) {
    Monitor(0),
    Window(1),
}

/**
 * 镜像档位，与原生层 KLIVE_MODE_* 对应。
 *
 * 除 [Fullscreen] 外都由原生层按鼠标 / 前台窗口实时解析采集源；
 * 缩放档位同样跟随鼠标所在显示器，并排除目标显示器。
 */
enum class MirrorMode(val code: Int, val label: String, val description: String) {
    Fullscreen(0, "全屏镜像", "镜像左侧选中的显示器或窗口"),
    PointerMonitor(1, "跟随鼠标屏幕", "实时镜像鼠标所在显示器，排除目标屏"),
    PointerWindow(2, "鼠标下窗口", "镜像鼠标下方最上层的窗口"),
    FocusWindow(3, "前台窗口", "镜像当前前台窗口"),
    Zoom2x(4, "2× 缩放", "以鼠标为中心放大鼠标所在显示器"),
    Zoom3x(5, "3× 缩放", "以鼠标为中心放大鼠标所在显示器"),
    Zoom4x(6, "4× 缩放", "以鼠标为中心放大鼠标所在显示器");

    companion object {
        fun fromCode(code: Int): MirrorMode = entries.firstOrNull { it.code == code } ?: Fullscreen
    }
}

/** 持久化在插件配置里的镜像设置；显示器用 index 表示，原生层负责映射到稳定身份。 */
data class MirrorSettings(
    val mode: MirrorMode = MirrorMode.Fullscreen,
    val sourceIndex: Int? = null,
    val targetIndex: Int? = null,
)

/** 一个可采集的顶层窗口，由原生层 klive_window_desc 转换而来。 */
data class WindowInfo(
    val handle: Long,
    val processId: Int,
    val title: String,
    val className: String,
    val processName: String,
    val width: Int,
    val height: Int,
) {
    val displayName: String
        get() = title.ifBlank { className }

    val resolutionLabel: String
        get() = "$width × $height"
}

data class MirrorRequest(
    val sourceIndex: Int,
    val targetIndex: Int,
    val scaleMode: ScaleMode = ScaleMode.Fit,
    val vsync: Boolean = true,
    val backend: CaptureBackend = CaptureBackend.DesktopDuplication,
    val windowHandle: Long = 0L,
    val mode: MirrorMode = MirrorMode.Fullscreen,
)

data class MirrorStats(
    val running: Boolean,
    val frames: Long,
    val fps: Double,
    val frameWidth: Int,
    val frameHeight: Int,
    val lastError: Int,
    val mode: MirrorMode = MirrorMode.Fullscreen,
    val sourceIndex: Int = -1,
) {
    companion object {
        val Idle = MirrorStats(
            running = false,
            frames = 0,
            fps = 0.0,
            frameWidth = 0,
            frameHeight = 0,
            lastError = 0,
        )
    }
}
