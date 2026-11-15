package moe.lolosia.klive.plugin

import kotlinx.serialization.Serializable

/**
 * 与 core 的 `ToneCurve.DefaultValues` 保持一致；旧配置没有这个键时用默认曲线。
 */
private val defaultToneCurve = listOf(0f, 0.35f, 0.7f, 0.9f, 0.975f, 0.9857f)

/**
 * 持久化在 `config/plugins/klive.yaml` 的镜像设置。
 *
 * 字段与 `klive_webview_config.settings_json` 的键一一对应；显示器存 DXGI 设备名
 * （`\\.\DISPLAY1`）与型号，热插拔后 index 会漂移，不能存 index。
 */
@Serializable
internal data class MirrorSettings(
    val mode: Int = 0,
    val sourceMonitor: String = "",
    val sourceFriendly: String = "",
    val targetMonitor: String = "",
    val targetFriendly: String = "",
    /** HDR 色调曲线控制点的纵坐标（横坐标固定在 0 / .35 / .7 / .9 / 1.2 / 1.5）。 */
    val toneCurve: List<Float> = defaultToneCurve,
)
