package moe.lolosia.klive.capture

import com.sun.jna.Memory
import moe.lolosia.klive.model.CaptureBackend
import moe.lolosia.klive.model.CaptureSourceKind
import moe.lolosia.klive.model.MirrorRequest
import moe.lolosia.klive.model.MirrorStats
import moe.lolosia.klive.model.MonitorInfo
import moe.lolosia.klive.model.WindowInfo

/**
 * 对原生镜像会话的薄封装。
 *
 * 一个实例对应进程内唯一的镜像会话（原生层是全局单例），
 * 重复创建实例不会重复加载 DLL。
 */
class MirrorEngine {

    private val library = KliveNativeLoader.library

    fun monitors(): List<MonitorInfo> {
        val capacity = 16
        val memory = Memory((KliveMonitorStruct.SIZE * capacity).toLong())
        val count = library.klive_enumerate_monitors(memory, capacity)
        if (count <= 0) return emptyList()

        return (0 until count).map { i ->
            val struct = KliveMonitorStruct(memory.share((i * KliveMonitorStruct.SIZE).toLong()))
            MonitorInfo(
                index = struct.index,
                name = struct.nameString(),
                friendlyName = struct.friendlyNameString(),
                adapterName = struct.adapterNameString(),
                x = struct.x,
                y = struct.y,
                width = struct.width,
                height = struct.height,
                isPrimary = struct.isPrimary != 0,
                adapterLuidLow = struct.adapterLuidLow,
                adapterLuidHigh = struct.adapterLuidHigh,
                refreshHz = struct.refreshHz,
            )
        }
    }

    fun windows(): List<WindowInfo> {
        val capacity = 128
        val memory = Memory((KliveWindowStruct.SIZE * capacity).toLong())
        val count = library.klive_enumerate_windows(memory, capacity)
        if (count <= 0) return emptyList()

        return (0 until count).map { i ->
            val struct = KliveWindowStruct(memory.share((i * KliveWindowStruct.SIZE).toLong()))
            WindowInfo(
                handle = struct.handle,
                processId = struct.processId,
                title = struct.titleString(),
                className = struct.classNameString(),
                processName = struct.processNameString(),
                width = struct.width,
                height = struct.height,
            )
        }
    }

    /** 启动镜像；成功返回 null，失败返回可展示的错误描述。 */
    fun start(request: MirrorRequest): String? {
        val config = KliveMirrorConfigStruct().apply {
            windowHandle = request.windowHandle
            sourceIndex = request.sourceIndex
            targetIndex = request.targetIndex
            scaleMode = request.scaleMode.code
            vsync = if (request.vsync) 1 else 0
            // 窗口只能走 WGC
            captureBackend = if (request.windowHandle != 0L) {
                CaptureBackend.WindowsGraphicsCapture.code
            } else {
                request.backend.code
            }
            sourceKind = if (request.windowHandle != 0L) {
                CaptureSourceKind.Window.code
            } else {
                CaptureSourceKind.Monitor.code
            }
        }
        val code = library.klive_start_mirror(config)
        if (code == 0) return null
        return lastMessage() ?: "启动镜像失败（错误码 $code：${errorText(code)}）"
    }

    fun stop() {
        library.klive_stop_mirror()
    }

    fun stats(): MirrorStats {
        val struct = KliveMirrorStatsStruct()
        library.klive_get_stats(struct)
        return MirrorStats(
            running = struct.running != 0,
            frames = struct.frames,
            fps = struct.fps,
            frameWidth = struct.frameWidth,
            frameHeight = struct.frameHeight,
            lastError = struct.lastError,
        )
    }

    fun lastMessage(): String? = library.klive_last_message()?.takeIf { it.isNotBlank() }

    fun shutdown() {
        library.klive_shutdown()
    }

    private fun errorText(code: Int): String = when (code) {
        -1 -> "参数无效"
        -2 -> "镜像已经在运行"
        -3 -> "当前没有镜像在运行"
        -4 -> "没有可用显示器"
        -5 -> "图形设备创建失败"
        -6 -> "桌面复制失败"
        -7 -> "镜像窗口创建失败"
        -8 -> "当前配置不受支持"
        -9 -> "图形采集失败"
        else -> "未知错误"
    }
}
