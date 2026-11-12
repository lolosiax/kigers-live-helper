import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    alias(libs.plugins.kotlinJvm)
    alias(libs.plugins.composeMultiplatform)
    alias(libs.plugins.composeCompiler)
}

group = "moe.lolosia.klive"
version = "0.1.0"

dependencies {
    implementation(project(":core"))

    implementation(compose.desktop.currentOs)
    implementation(libs.compose.material3)
    implementation(libs.jna)
    implementation(libs.kotlinx.coroutinesSwing)

    testImplementation(kotlin("test"))
}

// ---------------------------------------------------------------------------
// 原生核心：DXGI Desktop Duplication + D3D11 呈现（C ABI，见 native/include/klive.h）
// ---------------------------------------------------------------------------
val isWindows = System.getProperty("os.name").startsWith("Windows", ignoreCase = true)
val nativeSrcDir = layout.projectDirectory.dir("../native")
val nativeBuildDir = layout.buildDirectory.dir("native/cmake")
val nativeDll = nativeBuildDir.map { it.file("Release/klive_capture.dll") }

val nativeConfigure by tasks.registering(Exec::class) {
    group = "native"
    description = "用 CMake 配置 klive_capture 原生库（VS2022 / x64）"
    onlyIf { isWindows }
    inputs.files(
        nativeSrcDir.file("CMakeLists.txt"),
        fileTree(nativeSrcDir) { include("include/**", "src/**") },
    )
    outputs.file(nativeBuildDir.map { it.file("CMakeCache.txt") })
    commandLine(
        "cmake",
        "-S", nativeSrcDir.asFile.absolutePath,
        "-B", nativeBuildDir.get().asFile.absolutePath,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
    )
}

val nativeBuild by tasks.registering(Exec::class) {
    group = "native"
    description = "编译 klive_capture.dll（Release）"
    onlyIf { isWindows }
    dependsOn(nativeConfigure)
    inputs.dir(nativeSrcDir)
    outputs.file(nativeDll)
    commandLine(
        "cmake",
        "--build", nativeBuildDir.get().asFile.absolutePath,
        "--config", "Release",
        "--target", "klive_capture",
    )
}

// DLL 作为 jvm 资源打包，运行时由 KliveNativeLoader 解压到临时目录再加载。
val nativeResourceDir = layout.buildDirectory.dir("generated/nativeResources")
val copyNativeLib by tasks.registering(Copy::class) {
    group = "native"
    description = "把 klive_capture.dll 放进 jvm 资源目录"
    onlyIf { isWindows }
    dependsOn(nativeBuild)
    from(nativeDll)
    into(nativeResourceDir.map { it.dir("native") })
}

sourceSets {
    main {
        resources.srcDir(files(nativeResourceDir).builtBy(copyNativeLib))
    }
}

// 开发时直接指向构建产物，省去每次复制；打包后走资源解压路径。
tasks.withType<JavaExec>().configureEach {
    if (name == "run") {
        dependsOn(nativeBuild)
        systemProperty("klive.native.path", nativeDll.get().asFile.absolutePath)
    }
}

compose.desktop {
    application {
        mainClass = "moe.lolosia.klive.MainKt"
        jvmArgs("-Dfile.encoding=UTF-8")

        nativeDistributions {
            targetFormats(TargetFormat.Msi)
            packageName = "klive"
            packageVersion = "0.1.0"
        }
    }
}

// 无窗口自检：打印枚举到的显示器，用来验证原生库链路是否通。
tasks.register<JavaExec>("probe") {
    group = "verification"
    description = "枚举显示器后退出（不弹窗）"
    dependsOn(nativeBuild)
    classpath = sourceSets["main"].runtimeClasspath
    mainClass.set("moe.lolosia.klive.MainKt")
    systemProperty("klive.native.path", nativeDll.get().asFile.absolutePath)
    systemProperty("stdout.encoding", "UTF-8")
    args("--probe")
}
