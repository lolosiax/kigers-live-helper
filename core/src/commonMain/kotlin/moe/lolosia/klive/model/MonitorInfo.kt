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
)

data class MirrorStats(
    val running: Boolean,
    val frames: Long,
    val fps: Double,
    val frameWidth: Int,
    val frameHeight: Int,
    val lastError: Int,
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
