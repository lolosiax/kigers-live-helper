package moe.lolosia.klive.plugin

import kotlinx.serialization.Serializable

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
)
