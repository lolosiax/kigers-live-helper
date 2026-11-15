package moe.lolosia.klive.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
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
import androidx.compose.material3.AlertDialog
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
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import moe.lolosia.klive.capture.MirrorEngine
import moe.lolosia.klive.model.CaptureBackend
import moe.lolosia.klive.model.CaptureSourceKind
import moe.lolosia.klive.model.MirrorMode
import moe.lolosia.klive.model.MirrorRequest
import moe.lolosia.klive.model.MirrorSettings
import moe.lolosia.klive.model.MirrorStats
import moe.lolosia.klive.model.MonitorInfo
import moe.lolosia.klive.model.ScaleMode
import moe.lolosia.klive.model.ToneCurve
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
    var mode by remember { mutableStateOf(MirrorMode.Fullscreen) }
    var running by remember { mutableStateOf(false) }
    var stats by remember { mutableStateOf(MirrorStats.Idle) }
    var message by remember { mutableStateOf<String?>(null) }
    var settingsReady by remember { mutableStateOf(false) }
    /** HDR 色调曲线；控制点纵坐标可拖，LUT 实时推给原生层。 */
    var toneCurve by remember { mutableStateOf(ToneCurve.Default) }
    /** 正在调 HDR 参数的那台显示器；非空时弹出调参窗口。 */
    var hdrDialog by remember { mutableStateOf<MonitorInfo?>(null) }

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

    LaunchedEffect(Unit) {
        refresh()
        // 先把上次的选择贴回枚举结果，再允许回写，避免启动时用默认值覆盖配置
        val saved = runCatching { engine.settings() }.getOrNull()
        if (saved != null) {
            val indices = monitors.map { it.index }
            mode = saved.mode
            toneCurve = ToneCurve(saved.toneCurve)
            saved.sourceIndex?.takeIf { it in indices }?.let { sourceIndex = it }
            saved.targetIndex?.takeIf { it in indices }?.let { targetIndex = it }
            if (sourceIndex == targetIndex) {
                targetIndex = indices.firstOrNull { it != sourceIndex } ?: targetIndex
            }
        }
        // 原生层有同样的默认曲线，这里再推一次，保证面板看到的与画面上的一致
        engine.setToneCurve(toneCurve.lut())
        settingsReady = true
    }

    LaunchedEffect(settingsReady, mode, sourceIndex, targetIndex) {
        if (!settingsReady) return@LaunchedEffect
        engine.saveSettings(MirrorSettings(mode, sourceIndex, targetIndex, toneCurve.values))
    }

    LaunchedEffect(Unit) {
        while (true) {
            if (running) {
                runCatching { engine.stats() }.onSuccess { current ->
                    stats = current
                    if (!current.running) {
                        running = false
                        message = engine.lastMessage()
                    } else if (current.mode != mode) {
                        // 侧键在原生层切了档，面板跟上
                        mode = current.mode
                    }
                }
            }
            delay(300)
        }
    }

    val source = sourceIndex
    val target = targetIndex
    val canStart = !running && target != null && when {
        // 动态档位只需要一块「不是目标屏」的显示器
        mode != MirrorMode.Fullscreen -> monitors.any { it.index != target }
        sourceKind == CaptureSourceKind.Monitor -> source != null && source != target
        else -> windowHandle != null
    }

    Column(
        modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(24.dp),
    ) {
        Header(
            running = running,
            onRefresh = { scope.launch { refresh() } },
            onOpenConsole = {
                scope.launch {
                    val error = engine.openConsole()
                    if (error != null) message = error
                }
            },
        )
        Spacer(Modifier.height(16.dp))
        ModeBar(mode = mode, onSelect = { mode = it })
        Spacer(Modifier.height(16.dp))

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
                onOpenHdr = { hdrDialog = it },
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
                mode != MirrorMode.Fullscreen && monitors.size < 2 -> "至少需要两台显示器"
                mode == MirrorMode.Fullscreen && sourceKind == CaptureSourceKind.Monitor &&
                    monitors.size < 2 -> "至少需要两台显示器"
                mode == MirrorMode.Fullscreen && sourceKind == CaptureSourceKind.Window &&
                    windows.isEmpty() -> "没有找到可采集窗口"
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
                            mode = mode,
                        )
                        CaptureSourceKind.Window -> MirrorRequest(
                            sourceIndex = 0,
                            targetIndex = target ?: 0,
                            scaleMode = scaleMode,
                            vsync = vsync,
                            windowHandle = windowHandle ?: 0L,
                            mode = mode,
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

    val hdrMonitor = hdrDialog
    if (hdrMonitor != null) {
        HdrToneDialog(
            monitor = hdrMonitor,
            curve = toneCurve,
            onCurveChange = { curve ->
                toneCurve = curve
                scope.launch { engine.setToneCurve(curve.lut()) }
            },
            onDismiss = {
                hdrDialog = null
                // 拖曲线时只改运行参数，关闭弹窗才落盘
                scope.launch {
                    engine.saveSettings(
                        MirrorSettings(
                            mode = mode,
                            sourceIndex = sourceIndex,
                            targetIndex = targetIndex,
                            toneCurve = toneCurve.values,
                        ),
                    )
                }
            },
        )
    }
}

@Composable
private fun Header(running: Boolean, onRefresh: () -> Unit, onOpenConsole: () -> Unit) {
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
                color = MaterialTheme.colorScheme.onBackground,
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
        TextButton(onClick = onOpenConsole) { Text("打开终端") }
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
private fun ModeBar(mode: MirrorMode, onSelect: (MirrorMode) -> Unit) {
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "镜像档位",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.width(10.dp))
            MirrorMode.entries.forEach { item ->
                ModeChip(item.label, item == mode) { onSelect(item) }
                Spacer(Modifier.width(8.dp))
            }
        }
        Spacer(Modifier.height(6.dp))
        Text(
            "${mode.description} · 侧键：后退键上一档 / 前进键下一档",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
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
    onOpenHdr: (MonitorInfo) -> Unit,
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
                                onOptions = if (monitor.hdr) ({ onOpenHdr(monitor) }) else null,
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
private fun MonitorRow(
    monitor: MonitorInfo,
    selected: Boolean,
    accent: Color,
    onClick: () -> Unit,
    onOptions: (() -> Unit)? = null,
) {
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
                if (monitor.hdr) {
                    Spacer(Modifier.width(6.dp))
                    HdrBadge()
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
        if (onOptions != null) {
            // 单独的调参入口：点击不能连带选中这一行
            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(8.dp))
                    .clickable(onClick = onOptions)
                    .padding(horizontal = 8.dp, vertical = 2.dp),
            ) {
                Text(
                    // 用中间点拼三个点：楷体/等线里 ⋮ 这类数学符号不一定有字形
                    "···",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.width(4.dp))
        }
        Text(
            if (selected) "已选" else "选择",
            style = MaterialTheme.typography.labelSmall,
            color = if (selected) accent else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun HdrBadge() {
    val color = MaterialTheme.colorScheme.tertiary
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(color.copy(alpha = 0.16f))
            .padding(horizontal = 6.dp, vertical = 1.dp),
    ) {
        Text("HDR", style = MaterialTheme.typography.labelSmall, color = color)
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

/**
 * HDR 色调映射调参窗口。
 *
 * 曲线是采集侧（原生着色器）的全局参数：所有 HDR 采集源共用一份，
 * 因此从任意 HDR 显示器打开都是同一条。
 */
@Composable
private fun HdrToneDialog(
    monitor: MonitorInfo,
    curve: ToneCurve,
    onCurveChange: (ToneCurve) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("HDR 色调曲线") },
        text = {
            Column {
                Text(
                    "${monitor.displayName} · 所有 HDR 采集源共用",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(12.dp))
                ToneCurveEditor(
                    curve = curve,
                    onCurveChange = onCurveChange,
                    modifier = Modifier.fillMaxWidth().height(200.dp),
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "横轴：输入亮度 0–2（1.0 = SDR 白）· 纵轴：输出 0–1",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(10.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        "拖动圆点改曲线：往下压暗，往上是提亮。",
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    TextButton(onClick = { onCurveChange(ToneCurve.Default) }) { Text("恢复默认") }
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text("完成") } },
    )
}

/** 曲线编辑画布：控制点横坐标固定，拖动只改纵坐标。 */
@Composable
private fun ToneCurveEditor(
    curve: ToneCurve,
    onCurveChange: (ToneCurve) -> Unit,
    modifier: Modifier = Modifier,
) {
    val latest = rememberUpdatedState(curve)
    val accent = MaterialTheme.colorScheme.primary
    val grid = MaterialTheme.colorScheme.outline
    val handleFill = MaterialTheme.colorScheme.surface
    var active by remember { mutableStateOf(-1) }
    val lut = remember(curve) { curve.lut() }

    Canvas(
        modifier = modifier
            .clip(RoundedCornerShape(10.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.35f))
            .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(10.dp))
            .pointerInput(Unit) {
                val width = size.width.toFloat()
                val height = size.height.toFloat()
                fun pick(position: Offset): Int {
                    val points = latest.value.sanitized()
                    var best = 0
                    var bestDistance = Float.MAX_VALUE
                    points.forEachIndexed { index, value ->
                        val dx = width * ToneCurve.Knots[index] / ToneCurve.MaxInput - position.x
                        val dy = height * (1f - value) - position.y
                        val distance = dx * dx + dy * dy
                        if (distance < bestDistance) {
                            bestDistance = distance
                            best = index
                        }
                    }
                    return best
                }
                detectDragGestures(
                    onDragStart = { position -> active = pick(position) },
                    onDragEnd = { active = -1 },
                    onDragCancel = { active = -1 },
                    onDrag = { change, _ ->
                        val index = active
                        if (index >= 0) {
                            val values = latest.value.sanitized().toMutableList()
                            values[index] = (1f - change.position.y / height).coerceIn(0f, 1f)
                            onCurveChange(ToneCurve(values))
                            change.consume()
                        }
                    },
                )
            },
    ) {
        fun px(x: Float) = size.width * x / ToneCurve.MaxInput
        fun py(y: Float) = size.height * (1f - y.coerceIn(0f, 1f))

        // 网格（1/4 步进）与 SDR 白参考线
        for (step in 1 until 4) {
            val gx = px(ToneCurve.MaxInput * step / 4f)
            drawLine(grid.copy(alpha = 0.18f), Offset(gx, 0f), Offset(gx, size.height), 1.dp.toPx())
            val gy = py(step / 4f)
            drawLine(grid.copy(alpha = 0.18f), Offset(0f, gy), Offset(size.width, gy), 1.dp.toPx())
        }
        drawLine(grid.copy(alpha = 0.45f), Offset(px(1f), 0f), Offset(px(1f), size.height), 1.dp.toPx())

        // 对角线参考：输入 == 输出
        drawLine(grid.copy(alpha = 0.45f), Offset(px(0f), py(0f)), Offset(px(1f), py(1f)), 1.dp.toPx())

        val path = Path()
        lut.forEachIndexed { index, value ->
            val x = ToneCurve.MaxInput * index / (lut.size - 1)
            if (index == 0) path.moveTo(px(x), py(value)) else path.lineTo(px(x), py(value))
        }
        drawPath(path, accent, style = Stroke(width = 2.dp.toPx()))

        curve.sanitized().forEachIndexed { index, value ->
            val center = Offset(px(ToneCurve.Knots[index]), py(value))
            drawCircle(handleFill, 6.dp.toPx(), center)
            drawCircle(
                if (index == active) accent else handleFill,
                6.dp.toPx(),
                center,
                style = Stroke(width = 2.dp.toPx()),
            )
        }
    }
}
