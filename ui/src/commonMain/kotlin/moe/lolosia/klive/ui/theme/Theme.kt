package moe.lolosia.klive.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily

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
fun KliveTheme(fontFamily: FontFamily? = null, content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = KliveColorScheme,
        typography = kliveTypography(fontFamily),
        content = content,
    )
}

/**
 * wasm 没有系统字体，需要由外壳提供一份中文字体；桌面端传 null 走系统默认。
 */
private fun kliveTypography(fontFamily: FontFamily?): Typography {
    if (fontFamily == null) return Typography()
    val base = Typography()
    return Typography(
        displayLarge = base.displayLarge.copy(fontFamily = fontFamily),
        displayMedium = base.displayMedium.copy(fontFamily = fontFamily),
        displaySmall = base.displaySmall.copy(fontFamily = fontFamily),
        headlineLarge = base.headlineLarge.copy(fontFamily = fontFamily),
        headlineMedium = base.headlineMedium.copy(fontFamily = fontFamily),
        headlineSmall = base.headlineSmall.copy(fontFamily = fontFamily),
        titleLarge = base.titleLarge.copy(fontFamily = fontFamily),
        titleMedium = base.titleMedium.copy(fontFamily = fontFamily),
        titleSmall = base.titleSmall.copy(fontFamily = fontFamily),
        bodyLarge = base.bodyLarge.copy(fontFamily = fontFamily),
        bodyMedium = base.bodyMedium.copy(fontFamily = fontFamily),
        bodySmall = base.bodySmall.copy(fontFamily = fontFamily),
        labelLarge = base.labelLarge.copy(fontFamily = fontFamily),
        labelMedium = base.labelMedium.copy(fontFamily = fontFamily),
        labelSmall = base.labelSmall.copy(fontFamily = fontFamily),
    )
}
