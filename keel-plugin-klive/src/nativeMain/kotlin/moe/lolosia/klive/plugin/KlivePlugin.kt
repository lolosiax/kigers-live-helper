package moe.lolosia.klive.plugin

import moe.lolosia.keel.api.Log
import moe.lolosia.keel.api.Plugin
import moe.lolosia.keel.api.PluginContext
import moe.lolosia.keel.api.annotation.KeelPluginMain

/**
 * Kiger直播助手的 keel 入口：把 `.keel` 里的原生 DLL 与 wasm 面板物化出来，
 * 再交给 `klive_webview.dll` 打开 WebView2 窗口。
 *
 * 采集、呈现和桥协议都在原生层，这里只负责生命周期；`onEnable` 跑在
 * keel 的 Worker 线程上，正好满足 `materialize` 的线程要求。
 */
@KeelPluginMain
class KlivePlugin : Plugin {

    private var context: PluginContext? = null
    private var shell: WebViewShell? = null

    override fun onLoad(context: PluginContext) {
        this.context = context
        WebConsole.attach(context.server.messenger)
    }

    override fun onEnable() {
        val context = this.context ?: return
        if (shell != null) return

        val nativeDir = context.materialize("native")
        val webDir = context.materialize("web")
        Log.info("native=$nativeDir web=$webDir")

        val loaded = WebViewShell.load("$nativeDir\\klive_webview.dll")
        if (loaded == null) {
            // 抛出而不是静默返回：Host 会把插件标记为启动失败，比「面板不出现」好排查
            throw IllegalStateException("加载 klive_webview.dll 失败（$nativeDir）")
        }

        val code = loaded.start(nativeDir, webDir, devtools = true)
        if (code != 0) {
            val message = loaded.lastError()
            loaded.close()
            throw IllegalStateException("WebView2 启动失败 code=$code message=$message")
        }

        shell = loaded
        Log.info("控制面板已启动")
    }

    override fun onDisable() {
        val loaded = shell ?: return
        shell = null
        loaded.stop()
        loaded.close()
        Log.info("控制面板已停止")
    }
}
