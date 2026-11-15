package moe.lolosia.klive.plugin

import kotlinx.serialization.json.*
import moe.lolosia.keel.api.*
import moe.lolosia.keel.api.annotation.KeelPluginMain
import moe.lolosia.keel.api.config.*
import moe.lolosia.keel.api.messaging.*

/**
 * Kiger直播助手的 keel 入口：把 `.keel` 里的原生 DLL 与 wasm 面板物化出来，
 * 再交给 `klive_webview.dll` 打开 WebView2 窗口。
 *
 * 采集、呈现和桥协议都在原生层；镜像设置（档位 / 全屏档的显示器）
 * 存在插件配置 `config/plugins/klive.yaml`，由这里读写。
 * `onEnable` 跑在 keel 的 Worker 线程上，正好满足 `materialize` 的线程要求。
 */
@KeelPluginMain
class KlivePlugin : Plugin {

    private var context: PluginContext? = null
    private var shell: WebViewShell? = null

    override fun onLoad(context: PluginContext) {
        this.context = context
        WebConsole.attach(context.server.messenger)
        MirrorSettingsBridge.attach(context.server.messenger)
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

        val settings = runCatching { context.config.read<MirrorSettings>("mirror") }
            .onFailure {
                if (it is NoSuchElementException) {
                    Log.info("尚未保存镜像设置，使用默认值")
                } else {
                    Log.warn("读取镜像设置失败：${it::class.simpleName} ${it.message}")
                }
            }
            .getOrElse { MirrorSettings() }
        Log.info("镜像设置：$settings")
        val code = loaded.start(
            nativeDir = nativeDir,
            webDir = webDir,
            dataDir = context.dataDir,
            devtools = true,
            settingsJson = Json.encodeToString(MirrorSettings.serializer(), settings),
        )
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

    /**
     * 原生侧切档 / 改选后回传完整设置 JSON；这里在 Worker 线程落盘。
     * 配置写失败只记日志，不能让事件泵抛异常。
     */
    override fun onMessage(message: Message): ByteArray? {
        if (message.channel != MirrorSettingsBridge.CHANNEL) return null
        val context = this.context ?: return null

        runCatching {
            val text = message.payload.decodeToString()
            val settings = Json.decodeFromString(MirrorSettings.serializer(), text)
            context.config.write(settings, "mirror")
            context.config.save()
        }.onFailure {
            Log.warn("保存镜像设置失败：${it.message}")
        }
        return ByteArray(0)
    }
}
