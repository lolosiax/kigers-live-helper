package moe.lolosia.klive.ui.bridge

import kotlinx.serialization.Serializable

/** 与 `native/webview/klive_webview.cpp` 的 `BuildMonitors()` 字段一一对应。 */
@Serializable
internal data class MonitorDto(
    val index: Int,
    val name: String = "",
    val friendlyName: String = "",
    val adapterName: String = "",
    val x: Int = 0,
    val y: Int = 0,
    val width: Int = 0,
    val height: Int = 0,
    val isPrimary: Boolean = false,
    val adapterLuidLow: Int = 0,
    val adapterLuidHigh: Int = 0,
    val refreshHz: Float = 0f,
    val hdr: Boolean = false,
)

/** 窗口句柄是 64 位，JSON 里用字符串传，避免 JS number 精度损失。 */
@Serializable
internal data class WindowDto(
    val handle: String,
    val processId: Int = 0,
    val title: String = "",
    val className: String = "",
    val processName: String = "",
    val width: Int = 0,
    val height: Int = 0,
)

@Serializable
internal data class StatsDto(
    val running: Boolean = false,
    val frames: Long = 0,
    val fps: Double = 0.0,
    val frameWidth: Int = 0,
    val frameHeight: Int = 0,
    val lastError: Int = 0,
    val mode: Int = 0,
    val sourceIndex: Int = -1,
)
