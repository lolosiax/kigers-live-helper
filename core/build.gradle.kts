import org.jetbrains.kotlin.gradle.ExperimentalWasmDsl

plugins {
    alias(libs.plugins.kotlinMultiplatform)
}

/*
 * 跨平台共享模型：只有纯 Kotlin，不引用任何平台 API，也不带 Compose。
 *
 * `wasmJs` 给浏览器 / WebView2 里的 Compose 面板使用；
 * `mingwX64` / `linuxX64` 保证这层始终能编译到 keel 插件使用的目标上。
 */
kotlin {
    jvmToolchain(21)

    jvm()
    @OptIn(ExperimentalWasmDsl::class)
    wasmJs { browser() }
    mingwX64()
    linuxX64()

    sourceSets {
        commonTest.dependencies {
            implementation(kotlin("test"))
        }
    }
}
