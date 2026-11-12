package moe.lolosia.klive.ui.engine

import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.model.*

/**
 * 浏览器开发模式下的假数据：只用于调布局与交互，不碰任何原生层。
 */
class MockMirrorEngine : MirrorEngine {

    private var running = false
    private var frames = 0L

    private val sampleMonitors = listOf(
        MonitorInfo(
            index = 0,
            name = "\\\\.\\DISPLAY1",
            friendlyName = "DELL U2720Q",
            adapterName = "NVIDIA GeForce RTX 4070",
            x = 0,
            y = 0,
            width = 2560,
            height = 1440,
            isPrimary = true,
            adapterLuidLow = 1,
            adapterLuidHigh = 0,
            refreshHz = 60f,
        ),
        MonitorInfo(
            index = 1,
            name = "\\\\.\\DISPLAY2",
            friendlyName = "LG 27GL850",
            adapterName = "Intel UHD Graphics 770",
            x = 2560,
            y = 0,
            width = 1920,
            height = 1080,
            isPrimary = false,
            adapterLuidLow = 2,
            adapterLuidHigh = 0,
            refreshHz = 144f,
        ),
    )

    private val sampleWindows = listOf(
        WindowInfo(
            handle = 1001,
            processId = 4242,
            title = "Visual Studio Code",
            className = "Chrome_WidgetWin_1",
            processName = "Code.exe",
            width = 1600,
            height = 1000,
        ),
        WindowInfo(
            handle = 1002,
            processId = 5151,
            title = "任务管理器",
            className = "TaskManagerWindow",
            processName = "Taskmgr.exe",
            width = 900,
            height = 700,
        ),
    )

    override suspend fun monitors(): List<MonitorInfo> = sampleMonitors

    override suspend fun windows(): List<WindowInfo> = sampleWindows

    override suspend fun start(request: MirrorRequest): String? {
        running = true
        frames = 0
        return null
    }

    override suspend fun stop() {
        running = false
    }

    override suspend fun stats(): MirrorStats {
        if (running) frames += 18
        return MirrorStats(
            running = running,
            frames = frames,
            fps = if (running) 60.0 else 0.0,
            frameWidth = if (running) 2560 else 0,
            frameHeight = if (running) 1440 else 0,
            lastError = 0,
        )
    }

    override suspend fun lastMessage(): String? = null

    override suspend fun openConsole(): String? = null

    override fun shutdown() = Unit
}
