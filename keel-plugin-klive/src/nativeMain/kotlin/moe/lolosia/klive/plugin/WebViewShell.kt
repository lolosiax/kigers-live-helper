package moe.lolosia.klive.plugin

import klive_webview.klive_webview_config
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.CFunction
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.UShortVar
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.cstr
import kotlinx.cinterop.invoke
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.staticCFunction
import kotlinx.cinterop.toKString
import platform.windows.FreeLibrary
import platform.windows.GetFullPathNameW
import platform.windows.GetProcAddress
import platform.windows.HMODULE
import platform.windows.LoadLibraryExW

/**
 * `klive_webview.dll` 的动态加载器。
 *
 * 插件包里 DLL 已经物化到绝对目录，不能依赖 Windows 的默认搜索路径；
 * 用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` 让依赖的 `WebView2Loader.dll` 也在同一目录解析。
 */
@OptIn(ExperimentalForeignApi::class)
internal class WebViewShell private constructor(
    private val module: HMODULE,
    private val startFn: CPointer<CFunction<(CPointer<klive_webview_config>?) -> Int>>,
    private val stopFn: CPointer<CFunction<() -> Int>>,
    private val lastErrorFn: CPointer<CFunction<() -> CPointer<ByteVar>?>>,
) {

    /** 启动外壳；返回 0 表示窗口与 WebView2 已经就绪。 */
    fun start(nativeDir: String, webDir: String, devtools: Boolean): Int = memScoped {
        val config = alloc<klive_webview_config>()
        config.native_dir = nativeDir.cstr.ptr
        config.web_dir = webDir.cstr.ptr
        config.user_data_dir = null
        config.width = 1120
        config.height = 760
        config.min_width = 920
        config.min_height = 640
        config.enable_devtools = if (devtools) 1 else 0
        config.reserved = 0
        config.open_console = staticCFunction(::notifyOpenConsole)
        config.open_console_user = null
        startFn(config.ptr)
    }

    fun stop(): Int = stopFn()

    fun lastError(): String = lastErrorFn()?.toKString().orEmpty()

    fun close() {
        FreeLibrary(module)
    }

    companion object {
        private const val LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100u
        private const val LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000u

        fun load(dllPath: String): WebViewShell? = memScoped {
            // Host 的 plugins 目录可以是相对路径，materialize 会原样返回；
            // LoadLibraryExW 的 LOAD_LIBRARY_SEARCH_* 只认绝对路径。
            val path = absolutePath(dllPath)
            val module = LoadLibraryExW(
                path,
                null,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR or LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            ) ?: return@memScoped null

            val start = GetProcAddress(module, "klive_webview_start")
                ?.reinterpret<CFunction<(CPointer<klive_webview_config>?) -> Int>>()
            val stop = GetProcAddress(module, "klive_webview_stop")
                ?.reinterpret<CFunction<() -> Int>>()
            val lastError = GetProcAddress(module, "klive_webview_last_error")
                ?.reinterpret<CFunction<() -> CPointer<ByteVar>?>>()
            if (start == null || stop == null || lastError == null) {
                FreeLibrary(module)
                return@memScoped null
            }
            WebViewShell(module, start, stop, lastError)
        }

        private fun absolutePath(path: String): String {
            val required = GetFullPathNameW(path, 0u, null, null)
            if (required == 0u) return path
            return memScoped {
                val buffer = allocArray<UShortVar>(required.toInt() + 1)
                val written = GetFullPathNameW(path, required + 1u, buffer, null)
                if (written == 0u) path else buffer.toKString()
            }
        }
    }
}

/** WebView2 UI 线程点击「打开终端」时进入插件运行时；实现只投递消息，不能抛异常。 */
private fun notifyOpenConsole(@Suppress("UNUSED_PARAMETER") user: COpaquePointer?) {
    WebConsole.open()
}
