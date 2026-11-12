package moe.lolosia.klive.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.window.ComposeViewport
import kotlinx.browser.document
import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.ui.bridge.Bridge
import moe.lolosia.klive.ui.engine.MockMirrorEngine
import moe.lolosia.klive.ui.engine.WasmMirrorEngine
import moe.lolosia.klive.ui.font.loadSystemFont
import moe.lolosia.klive.ui.theme.KliveTheme

/**
 * wasm 面板入口：在 WebView2 里用真实桥，在浏览器里用 Mock，
 * 这样 `:ui:wasmJsBrowserDevelopmentRun` 不需要原生层也能调布局。
 */
@OptIn(ExperimentalComposeUiApi::class)
fun main() {
    val engine = if (Bridge.available()) WasmMirrorEngine(Bridge()) else MockMirrorEngine()
    ComposeViewport(document.body!!) {
        KliveRoot(engine)
    }
}

@Composable
private fun KliveRoot(engine: MirrorEngine) {
    var fontFamily by remember { mutableStateOf<FontFamily?>(null) }
    LaunchedEffect(Unit) { fontFamily = loadSystemFont() }
    KliveTheme(fontFamily = fontFamily) {
        App(engine)
    }
}
