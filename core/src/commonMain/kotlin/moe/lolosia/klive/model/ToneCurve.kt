package moe.lolosia.klive.model

import kotlin.math.sqrt

/**
 * HDR 色调曲线：线性输入 0..2（1.0 = SDR 白）映射到线性输出 0..1。
 *
 * 面板只暴露 [Knots] 这几个固定横坐标的控制点，纵坐标可拖；采样点由单调三次插值生成，
 * 保证控制点之间不会过冲。原生层按 [LutSize] 个采样点建一张 Texture1D 做查表。
 */
data class ToneCurve(val values: List<Float> = DefaultValues) {

    /** 长度不符时回落到默认曲线；合法输入夹到 0..1。 */
    fun sanitized(): List<Float> =
        if (values.size != Knots.size) DefaultValues else values.map { it.coerceIn(0f, 1f) }

    /** 生成 LUT：等距覆盖 0..[MaxInput]，交给 `klive_set_tone_curve`。 */
    fun lut(): List<Float> {
        val ys = sanitized()
        val xs = Knots
        val last = xs.size - 1
        val slope = FloatArray(last) { (ys[it + 1] - ys[it]) / (xs[it + 1] - xs[it]) }
        val tangent = FloatArray(xs.size)
        tangent[0] = slope[0]
        tangent[last] = slope[last - 1]
        for (i in 1 until last) {
            tangent[i] = if (slope[i - 1] * slope[i] <= 0f) 0f else (slope[i - 1] + slope[i]) / 2f
        }
        // Fritsch–Carlson：收紧切线，避免三次段在控制点之间过冲
        for (i in 0 until last) {
            if (slope[i] == 0f) {
                tangent[i] = 0f
                tangent[i + 1] = 0f
                continue
            }
            val a = tangent[i] / slope[i]
            val b = tangent[i + 1] / slope[i]
            val limit = a * a + b * b
            if (limit > 9f) {
                val scale = 3f / sqrt(limit)
                tangent[i] = scale * a * slope[i]
                tangent[i + 1] = scale * b * slope[i]
            }
        }

        return List(LutSize) { index ->
            val x = MaxInput * index / (LutSize - 1)
            sampleCurve(x, xs, ys, tangent).coerceIn(0f, 1f)
        }
    }

    companion object {
        /** 控制点横坐标（线性亮度），最后一点之外保持常数。 */
        val Knots = listOf(0f, 0.35f, 0.7f, 0.9f, 1.2f, 1.5f)

        /** 默认曲线：0.9 之前直通，之后压到 SDR 白附近，与原生默认 LUT 大致一致。 */
        val DefaultValues = listOf(0f, 0.35f, 0.7f, 0.9f, 0.975f, 0.9857f)

        const val MaxInput = 2f
        const val LutSize = 65

        val Default = ToneCurve()
    }
}

/** 单调三次 Hermite 求值；超出控制点范围按端点值取常数。 */
private fun sampleCurve(x: Float, xs: List<Float>, ys: List<Float>, tangents: FloatArray): Float {
    if (x <= xs.first()) return ys.first()
    if (x >= xs.last()) return ys.last()
    var index = 0
    while (index < xs.size - 2 && x > xs[index + 1]) ++index
    val h = xs[index + 1] - xs[index]
    val t = (x - xs[index]) / h
    val t2 = t * t
    val t3 = t2 * t
    return (2f * t3 - 3f * t2 + 1f) * ys[index] +
        (t3 - 2f * t2 + t) * h * tangents[index] +
        (-2f * t3 + 3f * t2) * ys[index + 1] +
        (t3 - t2) * h * tangents[index + 1]
}
