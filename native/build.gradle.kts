/*
 * 原生 DLL 的构建入口。
 *
 * C++ 源码不在 Gradle 的源码集里，这里只负责调 CMake：
 *   klive_capture.dll  采集核心（D3D11 / WGC）
 *   klive_webview.dll  WebView2 外壳 + JSON 桥
 *
 * 产物在 native/build/cmake/Release/，由 keel-plugin-klive 的 copyNative 取走。
 */
val isWindows = System.getProperty("os.name").startsWith("Windows", ignoreCase = true)
val cmakeSource = layout.projectDirectory
val cmakeBuild = layout.buildDirectory.dir("cmake")
val releaseDir = cmakeBuild.map { it.dir("Release") }

val nativeConfigure by tasks.registering(Exec::class) {
    group = "native"
    description = "用 CMake 配置原生工程（VS2022 / x64）"
    onlyIf { isWindows }
    inputs.files(
        cmakeSource.file("CMakeLists.txt"),
        fileTree(cmakeSource) { include("include/**", "src/**", "webview/**") },
    )
    outputs.file(cmakeBuild.map { it.file("CMakeCache.txt") })
    commandLine(
        "cmake",
        "-S", cmakeSource.asFile.absolutePath,
        "-B", cmakeBuild.get().asFile.absolutePath,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
    )
}

val nativeBuild by tasks.registering(Exec::class) {
    group = "native"
    description = "编译 klive_capture.dll / klive_webview.dll（Release）"
    onlyIf { isWindows }
    dependsOn(nativeConfigure)
    inputs.dir(cmakeSource)
    outputs.dir(releaseDir)
    commandLine(
        "cmake",
        "--build", cmakeBuild.get().asFile.absolutePath,
        "--config", "Release",
    )
}

/** WebView2 外壳的冒烟自检：8 秒后自动停止，桥日志在 stdout。 */
val nativeSmoke by tasks.registering(Exec::class) {
    group = "verification"
    description = "打开 WebView2 smoke 窗口并跑一轮桥协议（8 秒后退出）"
    onlyIf { isWindows }
    dependsOn(nativeBuild)
    commandLine(releaseDir.get().file("klive_webview_smoke.exe").asFile.absolutePath, "--exit-after=8000")
}
