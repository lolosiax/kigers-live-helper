pluginManagement {
    repositories {
        // keel 的工具链插件只发布到本机 ~/.m2；必须排在远程仓库前面
        mavenLocal()
        maven("https://maven.aliyun.com/repository/google") {
            mavenContent {
                includeGroupAndSubgroups("androidx")
                includeGroupAndSubgroups("com.android")
                includeGroupAndSubgroups("com.google")
            }
        }
        maven("https://maven.aliyun.com/repository/public")
        maven("https://maven.aliyun.com/repository/gradle-plugin")
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenLocal()
        maven("https://maven.aliyun.com/repository/public")
        // Compose Multiplatform 的桌面产物依赖 androidx.*（runtime-retain / savedstate / lifecycle）
        maven("https://maven.aliyun.com/repository/google") {
            mavenContent {
                includeGroupAndSubgroups("androidx")
                includeGroupAndSubgroups("com.android")
                includeGroupAndSubgroups("com.google")
            }
        }
        google()
        mavenCentral()
    }
}

rootProject.name = "klive"

include(":core")
include(":ui")
include(":native")
include(":keel-plugin-klive")
