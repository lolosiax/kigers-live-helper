package moe.lolosia.klive.capture

import com.sun.jna.Library
import com.sun.jna.Native
import com.sun.jna.Pointer
import com.sun.jna.Structure
import java.io.File

/**
 * klive_capture.dll 的 C ABI 绑定（见 native/include/klive.h）。
 *
 * 注意：方法名必须与导出的符号完全一致，所以这里不能是 internal
 * （Kotlin 会给 internal 成员加名字修饰）。
 */
interface KliveLibrary : Library {
    fun klive_initialize()

    fun klive_enumerate_monitors(out: Pointer, capacity: Int): Int

    fun klive_enumerate_windows(out: Pointer, capacity: Int): Int

    fun klive_start_mirror(config: KliveMirrorConfigStruct): Int

    fun klive_stop_mirror(): Int

    fun klive_is_running(): Int

    fun klive_get_stats(out: KliveMirrorStatsStruct)

    fun klive_last_message(): String?

    fun klive_shutdown()
}

@Structure.FieldOrder(
    "index", "x", "y", "width", "height", "isPrimary",
    "adapterLuidLow", "adapterLuidHigh", "refreshHz", "name", "friendlyName", "adapterName",
)
class KliveMonitorStruct : Structure {
    @JvmField var index: Int = 0
    @JvmField var x: Int = 0
    @JvmField var y: Int = 0
    @JvmField var width: Int = 0
    @JvmField var height: Int = 0
    @JvmField var isPrimary: Int = 0
    @JvmField var adapterLuidLow: Int = 0
    @JvmField var adapterLuidHigh: Int = 0
    @JvmField var refreshHz: Float = 0f
    @JvmField var name: ByteArray = ByteArray(64)
    @JvmField var friendlyName: ByteArray = ByteArray(96)
    @JvmField var adapterName: ByteArray = ByteArray(96)

    constructor() : super()

    constructor(pointer: Pointer) : super(pointer) {
        read()
    }

    fun nameString(): String = name.cString()
    fun friendlyNameString(): String = friendlyName.cString()
    fun adapterNameString(): String = adapterName.cString()

    companion object {
        val SIZE: Int = KliveMonitorStruct().size()
    }
}

@Structure.FieldOrder(
    "handle", "processId", "width", "height", "reserved", "title", "className", "processName",
)
class KliveWindowStruct : Structure {
    @JvmField var handle: Long = 0
    @JvmField var processId: Int = 0
    @JvmField var width: Int = 0
    @JvmField var height: Int = 0
    @JvmField var reserved: Int = 0
    @JvmField var title: ByteArray = ByteArray(160)
    @JvmField var className: ByteArray = ByteArray(96)
    @JvmField var processName: ByteArray = ByteArray(96)

    constructor() : super()

    constructor(pointer: Pointer) : super(pointer) {
        read()
    }

    fun titleString(): String = title.cString()
    fun classNameString(): String = className.cString()
    fun processNameString(): String = processName.cString()

    companion object {
        val SIZE: Int = KliveWindowStruct().size()
    }
}

@Structure.FieldOrder(
    "windowHandle", "sourceIndex", "targetIndex", "scaleMode", "vsync", "captureBackend", "sourceKind",
)
class KliveMirrorConfigStruct : Structure {
    @JvmField var windowHandle: Long = 0
    @JvmField var sourceIndex: Int = 0
    @JvmField var targetIndex: Int = 0
    @JvmField var scaleMode: Int = 0
    @JvmField var vsync: Int = 1
    @JvmField var captureBackend: Int = 0
    @JvmField var sourceKind: Int = 0

    constructor() : super()
}

@Structure.FieldOrder("frames", "fps", "frameWidth", "frameHeight", "running", "lastError")
class KliveMirrorStatsStruct : Structure {
    @JvmField var frames: Long = 0
    @JvmField var fps: Double = 0.0
    @JvmField var frameWidth: Int = 0
    @JvmField var frameHeight: Int = 0
    @JvmField var running: Int = 0
    @JvmField var lastError: Int = 0

    constructor() : super()
}

private fun ByteArray.cString(): String {
    val end = indexOf(0).let { if (it < 0) size else it }
    return copyOfRange(0, end).decodeToString()
}

/**
 * 原生库加载：
 * 1. `-Dklive.native.path` 指向构建产物（开发时用）；
 * 2. 否则从 jar 资源 `native/klive_capture.dll` 解压到临时目录。
 */
object KliveNativeLoader {
    private const val RESOURCE_PATH = "/native/klive_capture.dll"
    private const val FILE_NAME = "klive_capture.dll"

    val library: KliveLibrary by lazy {
        Native.load(resolvePath(), KliveLibrary::class.java).also { it.klive_initialize() }
    }

    private fun resolvePath(): String {
        System.getProperty("klive.native.path")
            ?.let(::File)
            ?.takeIf { it.isFile }
            ?.let { return it.absolutePath }
        return extractFromResources()
    }

    private fun extractFromResources(): String {
        val stream = KliveNativeLoader::class.java.getResourceAsStream(RESOURCE_PATH)
            ?: error("未找到原生库 $RESOURCE_PATH，请先运行 gradlew nativeBuild")

        val directory = File(System.getProperty("java.io.tmpdir"), "klive-native").apply { mkdirs() }
        val target = File(directory, FILE_NAME)
        stream.use { input ->
            target.outputStream().use { output -> input.copyTo(output) }
        }
        target.deleteOnExit()
        return target.absolutePath
    }
}
