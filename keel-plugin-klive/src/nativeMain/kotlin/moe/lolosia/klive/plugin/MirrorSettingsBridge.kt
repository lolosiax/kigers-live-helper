package moe.lolosia.klive.plugin

import moe.lolosia.keel.api.*
import moe.lolosia.keel.api.messaging.*
import kotlin.concurrent.Volatile

/**
 * 原生 `save_settings` 回调到 keel 事件泵的投递口。
 *
 * 回调在 WebView2 的 UI 线程上执行，只能做线程安全的单向 post；
 * 真正的 YAML 写入发生在插件 Worker 线程的 `onMessage` 里。
 */
internal object MirrorSettingsBridge {

    const val CHANNEL = "mirror-settings"

    private const val TARGET = "klive"

    @Volatile
    private var messenger: Messenger? = null

    fun attach(messenger: Messenger) {
        this.messenger = messenger
    }

    /** 把最新设置 JSON 投给本插件自己；插件未就绪时静默失败。 */
    fun post(json: String) {
        val target = messenger ?: return
        runCatching {
            target.post(
                Message(target = TARGET, channel = CHANNEL, payload = json.encodeToByteArray()),
            )
        }.onFailure {
            runCatching { Log.warn("投递镜像设置失败：${it.message}") }
        }
    }
}
