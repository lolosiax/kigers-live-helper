package moe.lolosia.klive

import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.ui.App
import moe.lolosia.klive.ui.theme.KliveTheme
import java.awt.Dimension

fun main(args: Array<String>) {
    if (args.contains("--probe")) {
        probe()
        return
    }

    application {
        val engine = remember { MirrorEngine() }

        Window(
            onCloseRequest = {
                engine.stop()
                exitApplication()
            },
            title = "klive — 屏幕镜像",
            state = rememberWindowState(width = 1120.dp, height = 760.dp),
        ) {
            LaunchedEffect(Unit) {
                window.minimumSize = Dimension(920, 640)
            }
            KliveTheme {
                App(engine)
            }
        }
    }
}

/** 无窗口自检：确认原生库可以加载并枚举出显示器。 */
private fun probe() {
    val engine = MirrorEngine()
    val monitors = engine.monitors()
    println("klive probe: 检测到 ${monitors.size} 台显示器")
    monitors.forEach { monitor ->
        val flags = buildString {
            if (monitor.isPrimary) append(" 主屏")
            if (monitor.index == monitors.first().index) append(" 首个")
        }
        println(
            "  [${monitor.index}] ${monitor.displayName}" +
                "  ${monitor.resolutionLabel} ${monitor.refreshLabel}" +
                "  位置 ${monitor.positionLabel}  显卡=${monitor.adapterName}  luid=${monitor.adapterKey}$flags",
        )
    }
    val windows = engine.windows()
    println("klive probe: 检测到 ${windows.size} 个可采集窗口")
    windows.take(12).forEach { window ->
        println("  ${window.processName.ifBlank { window.className }} · ${window.displayName} · ${window.resolutionLabel}")
    }
    engine.shutdown()
}
