# klive

把一台显示器的画面实时镜像到另一台显示器的 Windows 桌面工具。

首个 Demo 行为：控制面板里选择源显示器与目标显示器，点「开始镜像」，目标显示器会被一个
无边框置顶窗口铺满，实时显示源显示器的画面。

## 架构

| 层 | 技术 | 位置 |
| --- | --- | --- |
| 采集 + 呈现核心 | C++ / D3D11（桌面复制 + WGC 两种采集后端） | `native/` |
| 应用外壳 | Kotlin/JVM + Compose Desktop | `app/` |
| 共享模型 | Kotlin Multiplatform（jvm + mingwX64 + linuxX64，无 Compose） | `core/` |
| 桥接 | JNA 直连原生 C ABI | `app/src/main/kotlin/.../capture/` |

采集源支持显示器与窗口，采集后端支持：

| 后端 | 可用范围 | 特点 |
| --- | --- | --- |
| 桌面复制（DXGI Desktop Duplication） | 显示器 | 每台显示器独占，与 OBS 的「显示器采集」冲突 |
| WGC（Windows Graphics Capture） | 显示器 + 窗口 | 可多程序同时采集，窗口采集的唯一选择 |

两种后端都在 `native/src/frame_source.h` 里统一成 `FrameSource`，渲染与呈现逻辑共用。

原生库只暴露 C ABI（[klive.h](native/include/klive.h)），JVM 外壳用 JNA 调用；
将来若做 keel 插件（局域网 WebRTC/RTSP），同一份头文件可以直接给 Kotlin/Native cinterop 用。

`core` 与 `app` 拆开是必须的：Compose 编译器插件作用于整个 Gradle 工程，
一旦工程里再声明 mingwX64 / linuxX64 目标就会因为缺少 Compose 运行时而编译失败。
把纯 Kotlin 模型放在 `core`，才能既保证共享代码能编译到 keel 的目标上，又保住桌面 UI。

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
core/               共享模型：MonitorInfo / WindowInfo / CaptureBackend / MirrorRequest / MirrorStats
app/                Compose Desktop 控制面板 + JNA 绑定
```

## 构建与运行

要求：JDK 21、Visual Studio 2022（含 C++ 桌面工作负载）、CMake 3.20+。

```powershell
# 编译原生库（Release，输出 app/build/native/cmake/Release/klive_capture.dll）
.\gradlew.bat :app:nativeBuild

# 启动控制面板（会自动先构建原生库）
.\gradlew.bat :app:run

# 不开窗，只枚举显示器，用于排查原生链路
.\gradlew.bat :app:probe
```

打包 MSI：`.\gradlew.bat :app:packageMsi`（DLL 会作为资源打进 jar，运行时解压到临时目录再加载）。

## 使用

1. 左侧选采集源：切「显示器」或「窗口」；显示器还可切「桌面复制 / WGC」后端。右侧选目标显示器（可切换等比填充 / 拉伸铺满、垂直同步）。
2. 点「开始镜像」，目标显示器立即被镜像窗口覆盖。
3. 退出镜像：`Ctrl + Alt + Q`；镜像窗口拿到焦点时按 `Esc` 也可以。

## 限制

- 只支持 Windows（桌面复制需要 Win8+，WGC 需要 Win10 1903+，实测 Win11）。
- 桌面复制对每台显示器独占：OBS 之类正在用桌面复制捕获该显示器时会失败并提示，改用 WGC 后端即可共存；窗口采集只能走 WGC。
- 源显示器锁屏、切换分辨率、拔插显示器时复制会话会失效，代码里会自动重建，失败则报错。
- 源画面是 HDR（FP16 桌面格式）时目前按 SDR 直出，颜色可能偏亮，尚未做色调映射。

## 后续方向

- 把采集/呈现核心抽成 `core` 里的 expect/actual 实现，为 keel 插件复用做准备。
- 局域网串流（WebRTC / RTSP），镜像核心复用同一套帧获取逻辑。
- 鼠标指针绘制、多路镜像、热插拔监听。
