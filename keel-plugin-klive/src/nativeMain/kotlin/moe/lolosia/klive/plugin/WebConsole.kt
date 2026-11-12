package moe.lolosia.klive.plugin

import moe.lolosia.keel.api.Log
import moe.lolosia.keel.api.messaging.*
import kotlin.concurrent.Volatile

/**
 * 面板「打开终端」到 keel console 插件的控制通道。
 *
 * 窗口属于 console 插件，这里只发一条 fire-and-forget 消息。调用点可能在
 * WebView2 的 UI 线程上，因此只使用 Worker 初始化好的 [Messenger]
 * （底层是线程安全的事件队列），不触碰其它插件状态。
 */
internal object WebConsole {

    private const val TARGET = "console"
    private const val CHANNEL = "console"

    @Volatile
    private var messenger: Messenger? = null

    fun attach(messenger: Messenger) {
        this.messenger = messenger
    }

    /** 请求 console 插件打开窗口；插件不在线时静默失败。 */
    fun open() {
        val target = messenger ?: return
        runCatching {
            target.post(
                Message(target = TARGET, channel = CHANNEL, payload = "open".encodeToByteArray()),
            )
        }.onFailure {
            runCatching { Log.warn("打开控制台失败：${it.message}") }
        }
    }
}
