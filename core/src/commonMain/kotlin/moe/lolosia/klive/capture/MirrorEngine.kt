package moe.lolosia.klive.capture

import moe.lolosia.klive.model.MirrorRequest
import moe.lolosia.klive.model.MirrorStats
import moe.lolosia.klive.model.MonitorInfo
import moe.lolosia.klive.model.WindowInfo

/**
 * 控制面板依赖的镜像端口。
 *
 * 桌面外壳用 JNA 调 `klive_capture.dll` 实现；wasm 面板通过 WebView2 桥
 * 转发给 `klive_webview.dll`。实现都不允许抛异常来表达业务失败：
 * 启动失败以可展示的错误文案返回，查询失败才抛。
 */
interface MirrorEngine {

    suspend fun monitors(): List<MonitorInfo>

    suspend fun windows(): List<WindowInfo>

    /** 启动镜像；成功返回 null，失败返回可展示的错误描述。 */
    suspend fun start(request: MirrorRequest): String?

    suspend fun stop()

    suspend fun stats(): MirrorStats

    suspend fun lastMessage(): String?

    /** 请求打开 keel 控制台窗口；成功返回 null，失败返回可展示的错误描述。 */
    suspend fun openConsole(): String?

    fun shutdown()
}
