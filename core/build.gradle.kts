plugins {
    alias(libs.plugins.kotlinMultiplatform)
}

/*
 * 跨平台共享模型：只有纯 Kotlin，不引用任何平台 API，也不带 Compose。
 *
 * 声明 mingwX64 / linuxX64 是为了保证这层始终能编译到 keel 使用的目标上，
 * 将来把镜像/串流核心抽出来做成 keel 插件时可以直接复用。
 */
kotlin {
    jvmToolchain(21)

    jvm()
    mingwX64()
    linuxX64()

    sourceSets {
        commonTest.dependencies {
            implementation(kotlin("test"))
        }
    }
}
