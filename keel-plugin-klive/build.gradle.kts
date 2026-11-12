plugins {
    alias(libs.plugins.kotlinMultiplatform)
    id("moe.lolosia.keel.plugin") version "0.1.0-SNAPSHOT"
}

group = "moe.lolosia.klive"
version = "0.1.0"

keelPlugin {
    // keel 工件目前只有 0.1.0-SNAPSHOT；KSP / Host 都按这个版本解析
    keelVersion = libs.versions.keel.get()
    target("mingwX64")
    buildType = "DEBUG"
    resources("build/generated/pluginResources")
    // 桥日志默认关闭（每秒多条，会把 keel 控制台刷满）；需要时加 -Pklive.bridgeLog
    if (providers.gradleProperty("klive.bridgeLog").isPresent) {
        runEnv("KLIVE_WEBVIEW_LOG", "1")
    }
}

// keel-abi 静态库同样按 SNAPSHOT 版本从 mavenLocal 取
keelAbi {
    version = libs.versions.keel.get()
}

kotlin {
    mingwX64 {
        binaries.sharedLib { baseName = "keel-plugin-klive" }
        // 只取 klive_webview_config 的结构定义；函数仍然按绝对路径动态解析
        compilations.getByName("main").cinterops.create("klivewebview") {
            defFile(project.file("src/nativeInterop/cinterop/klive_webview.def"))
            includeDirs(rootProject.file("native/webview"))
        }
    }

    sourceSets {
        all {
            languageSettings.optIn("kotlinx.cinterop.ExperimentalForeignApi")
        }
        commonMain.dependencies {
            api(libs.keel.api)
        }
    }
}

// ---------------------------------------------------------------------------
// 插件资源：两个原生 DLL + wasm 面板
// ---------------------------------------------------------------------------
val copyNative by tasks.registering(Sync::class) {
    group = "keel"
    description = "把原生 DLL 复制进插件资源目录"
    dependsOn(":native:nativeBuild")
    from(rootProject.layout.projectDirectory.dir("native/build/cmake/Release")) {
        include("klive_capture.dll", "klive_webview.dll", "WebView2Loader.dll")
    }
    into(layout.buildDirectory.dir("generated/pluginResources/native"))
}

val copyWeb by tasks.registering(Sync::class) {
    group = "keel"
    description = "把 wasm 面板产物复制进插件资源目录"
    dependsOn(":ui:wasmJsBrowserDistribution")
    from(rootProject.layout.projectDirectory.dir("ui/build/dist/wasmJs/productionExecutable"))
    into(layout.buildDirectory.dir("generated/pluginResources/web"))
}

tasks.named("keelPackage") {
    dependsOn(copyNative, copyWeb)
}
