# Kiger's Live Helper（Kiger直播助手）

把一台显示器的画面实时镜像到另一台显示器的 Windows 桌面工具。
包名 `moe.lolosia.klive`、Gradle 工程名 `klive` 是缩写，产品名以本文件为准。

首个 Demo 行为：控制面板里选择源显示器与目标显示器，点「开始镜像」，目标显示器会被一个
无边框置顶窗口铺满，实时显示源显示器的画面。

## 架构

| 层 | 技术 | 位置 |
| --- | --- | --- |
| 采集 + 呈现核心 | C++ / D3D11（桌面复制 + WGC 两种采集后端） | `native/` |
| UI 外壳 | C++ / Win32 + WebView2（窗口、托盘、JSON 桥） | `native/webview/` |
| 控制面板 | Compose Multiplatform（wasmJs，跑在 WebView2 里） | `ui/` |
| 插件入口 | Kotlin/Native（keel 插件，负责物化资源与启动外壳） | `keel-plugin-klive/` |
| 共享模型 | Kotlin Multiplatform（jvm + wasmJs + mingwX64 + linuxX64，无 Compose） | `core/` |

采集源支持显示器与窗口，采集后端支持：

| 后端 | 可用范围 | 特点 |
| --- | --- | --- |
| 桌面复制（DXGI Desktop Duplication） | 显示器 | 每台显示器独占，与 OBS 的「显示器采集」冲突 |
| WGC（Windows Graphics Capture） | 显示器 + 窗口 | 可多程序同时采集，窗口采集的唯一选择 |

两种后端都在 `native/src/frame_source.h` 里统一成 `FrameSource`，渲染与呈现逻辑共用。

原生库只暴露 C ABI（[klive.h](native/include/klive.h)）；面板与原生之间是 WebView2 的
`postMessage` + JSON 桥（[klive_webview.h](native/webview/klive_webview.h)）。

`core` / `ui` / `keel-plugin-klive` 拆开是必须的：Compose 编译器插件作用于整个 Gradle 工程，
一旦工程里再声明 mingwX64 / linuxX64 目标就会因为缺少 Compose 运行时而编译失败。
把模型放 `core`、把 Compose 放 `ui`，keel 插件才能只依赖 `core`。

渲染路径全程在 GPU 上：桌面复制拿到 `ID3D11Texture2D` → `CopyResource` 到自己的纹理 →
D3D11 全屏四边形采样绘制 → flip 模型交换链 `Present`。CPU 不参与像素搬运。

设备建在**源显示器所属显卡**上。目标显示器挂在另一块显卡时，DWM 负责跨显卡合成
（本机已验证：RX 7900 XTX 上的 4K 主屏 → 核显上的 1080p 显示器，稳定 60fps）。

## 目录

```
native/             C++ 核心（CMake + MSVC）
  include/klive.h   C ABI 定义
  src/              显示器 / 窗口枚举、镜像会话、渲染循环
    frame_source.h  FrameSource 抽象 + 帧中转纹理
    klive_duplication.cpp  桌面复制后端
    klive_wgc.cpp          WGC 后端（C++/WinRT）
  webview/          Win32 + WebView2 外壳、托盘、JSON 桥
core/               共享模型 + MirrorEngine 端口
ui/                 Compose Multiplatform 控制面板（wasmJs / jvm）
keel-plugin-klive/  keel 插件：物化 DLL 与 wasm，拉起外壳
```

## 构建与运行

要求：JDK 21、Visual Studio 2022（含 C++ 桌面工作负载）、CMake 3.20+、WebView2 Runtime。

```powershell
# 编译原生库（输出 native/build/cmake/Release/*.dll）
.\gradlew.bat :native:nativeBuild

# 打出插件包 -> keel-plugin-klive/build/keel/keel-plugin-klive-0.1.0.keel
.\gradlew.bat :keel-plugin-klive:keelPackage

# 用 keel Host 加载插件并打开控制面板
.\gradlew.bat :keel-plugin-klive:keelRun
```

只调面板布局时可以直接在浏览器里跑（Mock 引擎，不碰原生层）：
`.\gradlew.bat :ui:wasmJsBrowserDevelopmentRun`。

## 使用

1. 左侧选采集源：切「显示器」或「窗口」；显示器还可切「桌面复制 / WGC」后端。右侧选目标显示器（可切换等比填充 / 拉伸铺满、垂直同步）。
2. 点「开始镜像」，目标显示器立即被镜像窗口覆盖。
3. 退出镜像：`Ctrl + Alt + Q`；镜像窗口拿到焦点时按 `Esc` 也可以。
4. 关闭面板只会缩到托盘；托盘菜单可以「显示面板 / 停止镜像 / 退出」。

## 限制

- 只支持 Windows（桌面复制需要 Win8+，WGC 需要 Win10 1903+，实测 Win11）。
- 桌面复制对每台显示器独占：OBS 之类正在用桌面复制捕获该显示器时会失败并提示，改用 WGC 后端即可共存；窗口采集只能走 WGC。
- 源显示器锁屏、切换分辨率、拔插显示器时复制会话会失效，代码里会自动重建，失败则报错。
- 源画面是 HDR（FP16 桌面格式）时目前按 SDR 直出，颜色可能偏亮，尚未做色调映射。

## 后续方向

- 局域网串流（WebRTC / RTSP），复用同一套帧获取逻辑。
- 鼠标指针绘制、多路镜像、热插拔监听。
