package moe.lolosia.klive.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.model.CaptureBackend
import moe.lolosia.klive.model.CaptureSourceKind
import moe.lolosia.klive.model.MirrorRequest
import moe.lolosia.klive.model.MirrorStats
import moe.lolosia.klive.model.MonitorInfo
import moe.lolosia.klive.model.ScaleMode
import moe.lolosia.klive.model.WindowInfo
import kotlin.math.roundToInt

@Composable
fun App(engine: MirrorEngine) {
    var monitors by remember { mutableStateOf<List<MonitorInfo>>(emptyList()) }
    var windows by remember { mutableStateOf<List<WindowInfo>>(emptyList()) }
    var loadError by remember { mutableStateOf<String?>(null) }
    var sourceKind by remember { mutableStateOf(CaptureSourceKind.Monitor) }
    var backend by remember { mutableStateOf(CaptureBackend.WindowsGraphicsCapture) }
    var sourceIndex by remember { mutableStateOf<Int?>(null) }
    var targetIndex by remember { mutableStateOf<Int?>(null) }
    var windowHandle by remember { mutableStateOf<Long?>(null) }
    var scaleMode by remember { mutableStateOf(ScaleMode.Fit) }
    var vsync by remember { mutableStateOf(true) }
    var running by remember { mutableStateOf(false) }
    var stats by remember { mutableStateOf(MirrorStats.Idle) }
    var message by remember { mutableStateOf<String?>(null) }

    val scope = rememberCoroutineScope()

    suspend fun refresh() {
        runCatching { engine.monitors() }
            .onSuccess { list ->
                loadError = null
                monitors = list
                val indices = list.map { it.index }
                val source = sourceIndex
                if (source == null || source !in indices) sourceIndex = indices.firstOrNull()
                val target = targetIndex
                if (target == null || target !in indices || target == sourceIndex) {
                    targetIndex = indices.firstOrNull { it != sourceIndex } ?: indices.firstOrNull()
                }
            }
            .onFailure { loadError = it.message ?: "枚举显示器失败" }

        runCatching { engine.windows() }
            .onSuccess { list ->
                windows = list
                if (windowHandle == null || list.none { it.handle == windowHandle }) {
                    windowHandle = list.firstOrNull()?.handle
                }
            }
            .onFailure { if (loadError == null) loadError = it.message ?: "枚举窗口失败" }
    }

    LaunchedEffect(Unit) { refresh() }

    LaunchedEffect(Unit) {
        while (true) {
            if (running) {
                runCatching { engine.stats() }.onSuccess { current ->
                    stats = current
                    if (!current.running) {
                        running = false
                        message = engine.lastMessage()
                    }
                }
            }
            delay(300)
        }
    }

    val source = sourceIndex
    val target = targetIndex
    val canStart = !running && target != null && when (sourceKind) {
        CaptureSourceKind.Monitor -> source != null && source != target
        CaptureSourceKind.Window -> windowHandle != null
    }

    Column(
        modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(24.dp),
    ) {
        Header(running = running, onRefresh = { scope.launch { refresh() } })
        Spacer(Modifier.height(20.dp))

        Row(modifier = Modifier.weight(1f).fillMaxWidth()) {
            SourcePanel(
                sourceKind = sourceKind,
                onSourceKindChange = { sourceKind = it },
                backend = backend,
                onBackendChange = { backend = it },
                monitors = monitors,
                windows = windows,
                selectedMonitor = sourceIndex,
                selectedWindow = windowHandle,
                onSelectMonitor = { sourceIndex = it },
                onSelectWindow = { windowHandle = it },
                modifier = Modifier.weight(1f).fillMaxHeight(),
            )
            MirrorArrow()
            MonitorPanel(
                title = "目标显示器",
                caption = "全屏镜像输出",
                monitors = monitors,
                selectedIndex = targetIndex,
                accent = MaterialTheme.colorScheme.secondary,
                onSelect = { targetIndex = it },
                modifier = Modifier.weight(1f).fillMaxHeight(),
            )
        }

        Spacer(Modifier.height(16.dp))
        OptionBar(
            scaleMode = scaleMode,
            vsync = vsync,
            onScaleModeChange = { scaleMode = it },
            onVsyncChange = { vsync = it },
        )

        val banner = loadError ?: message
        if (banner != null) {
            Spacer(Modifier.height(12.dp))
            MessageBar(banner)
        }

        Spacer(Modifier.height(12.dp))
        ControlBar(
            stats = stats,
            running = running,
            canStart = canStart,
            hint = when {
                sourceKind == CaptureSourceKind.Monitor && monitors.size < 2 -> "至少需要两台显示器"
                sourceKind == CaptureSourceKind.Window && windows.isEmpty() -> "没有找到可采集窗口"
                else -> null
            },
            onStart = {
                scope.launch {
                    val request = when (sourceKind) {
                        CaptureSourceKind.Monitor -> MirrorRequest(
                            sourceIndex = source ?: 0,
                            targetIndex = target ?: 0,
                            scaleMode = scaleMode,
                            vsync = vsync,
                            backend = backend,
                        )
                        CaptureSourceKind.Window -> MirrorRequest(
                            sourceIndex = 0,
                            targetIndex = target ?: 0,
                            scaleMode = scaleMode,
                            vsync = vsync,
                            windowHandle = windowHandle ?: 0L,
                        )
                    }
                    val error = engine.start(request)
                    message = error
                    running = error == null
                    stats = if (error == null) engine.stats() else MirrorStats.Idle
                }
            },
            onStop = {
                scope.launch {
                    engine.stop()
                    running = false
                    stats = engine.stats()
                }
            },
        )
    }
}

@Composable
private fun Header(running: Boolean, onRefresh: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier.size(38.dp)
                .clip(RoundedCornerShape(11.dp))
                .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.16f))
                .border(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.5f), RoundedCornerShape(11.dp)),
            contentAlignment = Alignment.Center,
        ) {
            Text("K", color = MaterialTheme.colorScheme.primary, fontWeight = FontWeight.Bold, fontSize = 18.sp)
        }
        Spacer(Modifier.width(12.dp))
        Column {
            Text(
                "Kiger直播助手",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                "Kiger's Live Helper · 桌面复制 + WGC",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.weight(1f))
        StatusPill(running)
        Spacer(Modifier.width(8.dp))
        TextButton(onClick = onRefresh) { Text("重新枚举") }
    }
}

@Composable
private fun StatusPill(running: Boolean) {
    val color = if (running) MaterialTheme.colorScheme.secondary else MaterialTheme.colorScheme.onSurfaceVariant
    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(color.copy(alpha = 0.14f))
            .padding(horizontal = 12.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(7.dp).clip(CircleShape).background(color))
        Spacer(Modifier.width(7.dp))
        Text(if (running) "正在镜像" else "空闲", style = MaterialTheme.typography.labelMedium, color = color)
    }
}

@Composable
private fun SourcePanel(
    sourceKind: CaptureSourceKind,
    onSourceKindChange: (CaptureSourceKind) -> Unit,
    backend: CaptureBackend,
    onBackendChange: (CaptureBackend) -> Unit,
    monitors: List<MonitorInfo>,
    windows: List<WindowInfo>,
    selectedMonitor: Int?,
    selectedWindow: Long?,
    onSelectMonitor: (Int) -> Unit,
    onSelectWindow: (Long) -> Unit,
    modifier: Modifier = Modifier,
) {
    val accent = MaterialTheme.colorScheme.primary
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
    ) {
        Column(Modifier.fillMaxSize().padding(18.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(6.dp).clip(CircleShape).background(accent))
                Spacer(Modifier.width(8.dp))
                Text("采集源", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                Text(
                    "被采集的画面",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(12.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                ModeChip("显示器", sourceKind == CaptureSourceKind.Monitor) {
                    onSourceKindChange(CaptureSourceKind.Monitor)
                }
                Spacer(Modifier.width(8.dp))
                ModeChip("窗口", sourceKind == CaptureSourceKind.Window) {
                    onSourceKindChange(CaptureSourceKind.Window)
                }
                Spacer(Modifier.weight(1f))
                if (sourceKind == CaptureSourceKind.Monitor) {
                    CaptureBackend.entries.forEach { mode ->
                        ModeChip(mode.label, mode == backend) { onBackendChange(mode) }
                        Spacer(Modifier.width(8.dp))
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            Text(
                when (sourceKind) {
                    CaptureSourceKind.Monitor -> backend.description
                    CaptureSourceKind.Window -> "窗口采集固定使用 WGC，可与其它采集程序共存"
                },
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(14.dp))

            if (sourceKind == CaptureSourceKind.Monitor) {
                if (monitors.isEmpty()) {
                    EmptyHint("没有检测到显示器")
                } else {
                    Column(
                        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        monitors.forEach { monitor ->
                            MonitorRow(
                                monitor = monitor,
                                selected = monitor.index == selectedMonitor,
                                accent = accent,
                                onClick = { onSelectMonitor(monitor.index) },
                            )
                        }
                    }
                }
            } else {
                if (windows.isEmpty()) {
                    EmptyHint("没有找到可采集窗口")
                } else {
                    Column(
                        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        windows.forEach { window ->
                            WindowRow(
                                window = window,
                                selected = window.handle == selectedWindow,
                                accent = accent,
                                onClick = { onSelectWindow(window.handle) },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun EmptyHint(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(
            text,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun WindowRow(window: WindowInfo, selected: Boolean, accent: Color, onClick: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    val background = if (selected) accent.copy(alpha = 0.12f) else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f)
    val borderColor = if (selected) accent.copy(alpha = 0.65f) else MaterialTheme.colorScheme.outline

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(background)
            .border(1.dp, borderColor, shape)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(34.dp)
                .clip(RoundedCornerShape(9.dp))
                .background(if (selected) accent.copy(alpha = 0.22f) else MaterialTheme.colorScheme.surface),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                window.processName.take(1).uppercase().ifBlank { "?" },
                style = MaterialTheme.typography.titleMedium,
                color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.SemiBold,
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                window.displayName,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                buildString {
                    append(window.processName.ifBlank { window.className })
                    append(" · ")
                    append(window.resolutionLabel)
                },
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
        Spacer(Modifier.width(8.dp))
        Text(
            if (selected) "已选" else "选择",
            style = MaterialTheme.typography.labelSmall,
            color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun MonitorPanel(
    title: String,
    caption: String,
    monitors: List<MonitorInfo>,
    selectedIndex: Int?,
    accent: Color,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
    ) {
        Column(Modifier.fillMaxSize().padding(18.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(6.dp).clip(CircleShape).background(accent))
                Spacer(Modifier.width(8.dp))
                Text(title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                Text(
                    caption,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(14.dp))

            if (monitors.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        "没有检测到显示器",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            } else {
                Column(
                    modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    monitors.forEach { monitor ->
                        MonitorRow(
                            monitor = monitor,
                            selected = monitor.index == selectedIndex,
                            accent = accent,
                            onClick = { onSelect(monitor.index) },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun MonitorRow(monitor: MonitorInfo, selected: Boolean, accent: Color, onClick: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    val background = if (selected) accent.copy(alpha = 0.12f) else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.45f)
    val borderColor = if (selected) accent.copy(alpha = 0.65f) else MaterialTheme.colorScheme.outline

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(background)
            .border(1.dp, borderColor, shape)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(34.dp)
                .clip(RoundedCornerShape(9.dp))
                .background(if (selected) accent.copy(alpha = 0.22f) else MaterialTheme.colorScheme.surface),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                "${monitor.index + 1}",
                style = MaterialTheme.typography.titleMedium,
                color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.SemiBold,
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    monitor.displayName,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Medium,
                )
                if (monitor.isPrimary) {
                    Spacer(Modifier.width(6.dp))
                    PrimaryBadge()
                }
            }
            Spacer(Modifier.height(2.dp))
            Text(
                "${monitor.resolutionLabel} · ${monitor.refreshLabel}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (monitor.adapterName.isNotBlank()) {
                Text(
                    monitor.adapterName,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                )
            }
        }
        Spacer(Modifier.width(8.dp))
        Text(
            if (selected) "已选" else "选择",
            style = MaterialTheme.typography.labelSmall,
            color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun PrimaryBadge() {
    val color = MaterialTheme.colorScheme.secondary
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(color.copy(alpha = 0.16f))
            .padding(horizontal = 6.dp, vertical = 1.dp),
    ) {
        Text("主屏", style = MaterialTheme.typography.labelSmall, color = color)
    }
}

@Composable
private fun MirrorArrow() {
    Column(
        modifier = Modifier.width(68.dp).fillMaxHeight(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            "→",
            fontSize = 26.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "镜像",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun OptionBar(
    scaleMode: ScaleMode,
    vsync: Boolean,
    onScaleModeChange: (ScaleMode) -> Unit,
    onVsyncChange: (Boolean) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            "缩放",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.width(10.dp))
        ScaleMode.entries.forEach { mode ->
            ModeChip(mode.label, mode == scaleMode) { onScaleModeChange(mode) }
            Spacer(Modifier.width(8.dp))
        }

        Spacer(Modifier.width(20.dp))
        Text(
            "垂直同步",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.width(6.dp))
        Switch(checked = vsync, onCheckedChange = onVsyncChange)

        Spacer(Modifier.weight(1f))
        Text(
            "退出镜像：Ctrl + Alt + Q（窗口聚焦时 Esc 亦可）",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun ModeChip(label: String, selected: Boolean, onClick: () -> Unit) {
    val accent = MaterialTheme.colorScheme.primary
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(if (selected) accent.copy(alpha = 0.16f) else MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f))
            .border(
                1.dp,
                if (selected) accent.copy(alpha = 0.6f) else MaterialTheme.colorScheme.outline,
                RoundedCornerShape(8.dp),
            )
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 6.dp),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun MessageBar(text: String) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(10.dp),
        color = MaterialTheme.colorScheme.errorContainer,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.error.copy(alpha = 0.5f)),
    ) {
        Text(
            text,
            modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onErrorContainer,
        )
    }
}

@Composable
private fun ControlBar(
    stats: MirrorStats,
    running: Boolean,
    canStart: Boolean,
    hint: String?,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            StatBlock("帧率", if (running) "${stats.fps.roundToInt()} fps" else "—")
            Spacer(Modifier.width(28.dp))
            StatBlock("画面", if (stats.frameWidth > 0) "${stats.frameWidth} × ${stats.frameHeight}" else "—")
            Spacer(Modifier.width(28.dp))
            StatBlock("累计帧", if (running) stats.frames.toString() else "—")

            Spacer(Modifier.weight(1f))
            hint?.let {
                Text(
                    it,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.width(14.dp))
            }
            if (running) {
                OutlinedButton(onClick = onStop) { Text("停止镜像") }
            } else {
                Button(onClick = onStart, enabled = canStart) { Text("开始镜像") }
            }
        }
    }
}

@Composable
private fun StatBlock(label: String, value: String) {
    Column {
        Text(
            label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            value,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
    }
}
