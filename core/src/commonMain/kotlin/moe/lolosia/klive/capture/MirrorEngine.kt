package moe.lolosia.klive.capture

import moe.lolosia.klive.model.MirrorRequest
import moe.lolosia.klive.model.MirrorSettings
import moe.lolosia.klive.model.MirrorStats
import moe.lolosia.klive.model.MonitorInfo
import moe.lolosia.klive.model.ToneCurve
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

    /** 读上次保存的档位与显示器选择；没有配置或读取失败返回 null。 */
    suspend fun settings(): MirrorSettings?

    /** 保存档位与显示器选择；运行中改档位会立即生效。失败静默。 */
    suspend fun saveSettings(settings: MirrorSettings)

    /**
     * 只把 HDR 色调曲线的 LUT（[ToneCurve.LutSize] 个采样点）推给原生层，下一帧生效、不落盘。
     *
     * 面板拖曲线时高频调用，因此与 [saveSettings] 分开；关闭弹窗后再调
     * [saveSettings] 持久化控制点。
     */
    suspend fun setToneCurve(lut: List<Float>)

    /** 请求打开 keel 控制台窗口；成功返回 null，失败返回可展示的错误描述。 */
    suspend fun openConsole(): String?

    fun shutdown()
}
