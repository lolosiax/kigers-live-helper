package moe.lolosia.klive.ui.engine

import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.*
import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.model.*
import moe.lolosia.klive.ui.bridge.Bridge
import moe.lolosia.klive.ui.bridge.MonitorDto
import moe.lolosia.klive.ui.bridge.StatsDto
import moe.lolosia.klive.ui.bridge.WindowDto

/**
 * 通过 WebView2 桥把控制面板的请求转发给 `klive_webview.dll`。
 *
 * 错误文案全部来自原生侧的 `klive_last_message()`，这里只做 JSON 包装，
 * 避免两边各维护一份错误描述。
 */
class WasmMirrorEngine(private val bridge: Bridge) : MirrorEngine {

    private val json = Json { ignoreUnknownKeys = true }

    override suspend fun monitors(): List<MonitorInfo> {
        val result = bridge.call("enumerateMonitors")
        return json.decodeFromJsonElement(ListSerializer(MonitorDto.serializer()), result).map { dto ->
            MonitorInfo(
                index = dto.index,
                name = dto.name,
                friendlyName = dto.friendlyName,
                adapterName = dto.adapterName,
                x = dto.x,
                y = dto.y,
                width = dto.width,
                height = dto.height,
                isPrimary = dto.isPrimary,
                adapterLuidLow = dto.adapterLuidLow,
                adapterLuidHigh = dto.adapterLuidHigh,
                refreshHz = dto.refreshHz,
                hdr = dto.hdr,
            )
        }
    }

    override suspend fun windows(): List<WindowInfo> {
        val result = bridge.call("enumerateWindows")
        return json.decodeFromJsonElement(ListSerializer(WindowDto.serializer()), result).map { dto ->
            WindowInfo(
                handle = dto.handle.toLongOrNull() ?: 0L,
                processId = dto.processId,
                title = dto.title,
                className = dto.className,
                processName = dto.processName,
                width = dto.width,
                height = dto.height,
            )
        }
    }

    override suspend fun start(request: MirrorRequest): String? {
        val params = buildJsonObject {
            put("windowHandle", request.windowHandle.toString())
            put("sourceIndex", request.sourceIndex)
            put("targetIndex", request.targetIndex)
            put("scaleMode", request.scaleMode.code)
            put("vsync", request.vsync)
            put("captureBackend", request.backend.code)
            put("sourceKind", request.sourceKindCode)
            put("mode", request.mode.code)
        }
        return runCatching { bridge.call("startMirror", params) }
            .exceptionOrNull()
            ?.let { it.message ?: "启动镜像失败" }
    }

    override suspend fun stop() {
        runCatching { bridge.call("stopMirror") }
    }

    override suspend fun stats(): MirrorStats {
        val result = bridge.call("getStats")
        val dto = json.decodeFromJsonElement(StatsDto.serializer(), result)
        return MirrorStats(
            running = dto.running,
            frames = dto.frames,
            fps = dto.fps,
            frameWidth = dto.frameWidth,
            frameHeight = dto.frameHeight,
            lastError = dto.lastError,
            mode = MirrorMode.fromCode(dto.mode),
            sourceIndex = dto.sourceIndex,
        )
    }

    override suspend fun settings(): MirrorSettings? = runCatching {
        val result = bridge.call("getSettings").jsonObject
        MirrorSettings(
            mode = MirrorMode.fromCode(result["mode"]?.jsonPrimitive?.intOrNull ?: 0),
            sourceIndex = result["sourceIndex"]?.jsonPrimitive?.intOrNull?.takeIf { it >= 0 },
            targetIndex = result["targetIndex"]?.jsonPrimitive?.intOrNull?.takeIf { it >= 0 },
            toneCurve = result["toneCurve"]?.jsonArray
                ?.mapNotNull { it.jsonPrimitive.floatOrNull }
                ?.takeIf { it.isNotEmpty() }
                ?: ToneCurve.DefaultValues,
        )
    }.getOrNull()

    override suspend fun saveSettings(settings: MirrorSettings) {
        runCatching {
            bridge.call(
                "saveSettings",
                buildJsonObject {
                    put("mode", settings.mode.code)
                    put("sourceIndex", settings.sourceIndex ?: -1)
                    put("targetIndex", settings.targetIndex ?: -1)
                    putJsonArray("toneCurve") { settings.toneCurve.forEach { add(it) } }
                },
            )
        }
    }

    override suspend fun setToneCurve(lut: List<Float>) {
        runCatching {
            bridge.call(
                "setToneCurve",
                buildJsonObject { putJsonArray("lut") { lut.forEach { add(it) } } },
            )
        }
    }

    override suspend fun lastMessage(): String? {
        val result = runCatching { bridge.call("lastMessage") }.getOrNull() ?: return null
        return result.jsonPrimitive.contentOrNull?.takeIf { it.isNotBlank() }
    }

    override suspend fun openConsole(): String? =
        runCatching { bridge.call("openConsole") }
            .exceptionOrNull()
            ?.let { it.message ?: "打开终端失败" }

    override fun shutdown() = Unit

    private val MirrorRequest.sourceKindCode: Int
        get() = if (windowHandle != 0L) CaptureSourceKind.Window.code else CaptureSourceKind.Monitor.code
}
