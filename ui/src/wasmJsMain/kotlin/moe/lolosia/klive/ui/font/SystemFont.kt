@file:OptIn(kotlin.js.ExperimentalWasmJsInterop::class)

package moe.lolosia.klive.ui.font

import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.platform.Font
import kotlinx.browser.window
import kotlinx.coroutines.await
import org.khronos.webgl.Int8Array
import org.khronos.webgl.toByteArray

/**
 * 载入外壳提供的中文字体。
 *
 * Compose 的 wasm 渲染没有系统字体管理器，不载入就会把中文画成豆腐块；
 * `klive_webview.dll` 把系统字体（等线 / 微软雅黑）当作普通资源发出来。
 * 浏览器开发模式下没有这个端点，失败时返回 null，继续用默认字体。
 */
suspend fun loadSystemFont(): FontFamily? = runCatching {
    // 把页面的查询串透传过去，便于用 ?font=simsun 做点阵字对比
    val query = window.location.search
    val url = if (query.startsWith("?")) {
        "https://klive.local/system-font$query"
    } else {
        "https://klive.local/system-font"
    }
    val response = window.fetch(url).await()
    if (!response.ok) return@runCatching null
    val bytes = Int8Array(response.arrayBuffer().await()).toByteArray()
    if (bytes.isEmpty()) null else FontFamily(Font("klive-system", bytes))
}.getOrNull()
