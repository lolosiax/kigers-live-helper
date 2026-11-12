package moe.lolosia.klive.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/** 深色“控制台”配色：低饱和背景 + 单一强调蓝，适合常驻的系统工具窗口。 */
private val KliveColorScheme = darkColorScheme(
    primary = Color(0xFF4A8FE7),
    onPrimary = Color(0xFF07111F),
    primaryContainer = Color(0xFF16304F),
    onPrimaryContainer = Color(0xFFD6E6FF),

    secondary = Color(0xFF5BB8B2),
    onSecondary = Color(0xFF042120),
    secondaryContainer = Color(0xFF123A38),
    onSecondaryContainer = Color(0xFFC8EFEC),

    background = Color(0xFF0E1014),
    onBackground = Color(0xFFE7EAF0),

    surface = Color(0xFF15181E),
    onSurface = Color(0xFFE7EAF0),

    surfaceVariant = Color(0xFF1D222A),
    onSurfaceVariant = Color(0xFF98A2B0),

    outline = Color(0xFF2B323C),
    outlineVariant = Color(0xFF20262E),

    error = Color(0xFFE05D5D),
    onError = Color(0xFF1A0A0A),
    errorContainer = Color(0xFF3A1A1A),
    onErrorContainer = Color(0xFFFFD9D9),
)

@Composable
fun KliveTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = KliveColorScheme, content = content)
}
