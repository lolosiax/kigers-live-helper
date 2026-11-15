<div align="center">

<img src="doc/logo/klive-icon.png" width="72" alt="Kiger's Live Helper" />

# Kiger's Live Helper

**Windows 桌面屏幕镜像工具**

把一台显示器（或一个窗口）的画面实时镜像到另一台显示器。

[Gitee](https://gitee.com/lolosia/kigers-live-helper) · [GitHub](https://github.com/lolosiax/kigers-live-helper) · [项目记忆](doc/项目记忆.md)

[快速开始](#快速开始) · [架构](#架构) · [构建任务](#构建任务) · [当前状态](#当前状态) · [进一步阅读](#进一步阅读)

[![Kotlin](https://img.shields.io/badge/Kotlin-2.4.0-7F52FF?logo=kotlin&logoColor=white)](https://kotlinlang.org)
[![Compose Multiplatform](https://img.shields.io/badge/Compose%20Multiplatform-1.11.1-4285F4)](https://www.jetbrains.com/compose-multiplatform/)
[![Kotlin/Native](https://img.shields.io/badge/Kotlin%2FNative-mingwX64-7F52FF)](https://kotlinlang.org/docs/native-overview.html)
[![WebView2](https://img.shields.io/badge/WebView2-Runtime-0078D4)](https://developer.microsoft.com/microsoft-edge/webview2/)
[![Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D4?logo=windows)](https://www.microsoft.com/windows)
[![keel](https://img.shields.io/badge/keel-0.1.0--SNAPSHOT-4A8FE7)](https://gitee.com/lolosia/keel)

</div>

- **形态**：C++/D3D11 原生核心 + keel 插件 + Compose wasmJs 控制面板，Kotlin 侧只经 C ABI 接触原生。
- **采集**：桌面复制（DXGI Desktop Duplication）与 WGC（Windows Graphics Capture）两种后端，支持显示器与窗口。
- **呈现**：渲染全程在 GPU 上，flip 模型交换链输出，支持等比填充 / 拉伸铺满与垂直同步。
- **交付**：既可以作为 `.keel` 插件安装，也可以产出内嵌插件 bundle 的单文件 `KigerLiveHelper.exe`（桌面应用模式）。

> 代码风格与硬性约定见 [AGENTS.md](AGENTS.md)，当前进度与踩坑清单见 [doc/项目记忆.md](doc/项目记忆.md)。

## 核心特性

**采集**

- 显示器采集支持桌面复制与 WGC：前者性能最好但每台显示器独占，后者可与 OBS 等程序共存。
- 窗口采集固定走 WGC，可采前台窗口与普通应用窗口。
- 两种后端统一为 `FrameSource`，渲染循环不感知后端差异。
- 桌面复制遇到 `DXGI_ERROR_ACCESS_LOST` 自动重建；WGC 在内容尺寸变化时重建帧池。

**呈现**

- 桌面复制 / WGC 的帧纹理先 `CopyResource` 到自有纹理，渲染循环只认稳定的 `ID3D11Texture2D`。
- D3D11 全屏四边形 + 线性采样 + `D3D11_CULL_NONE` + 显式 viewport + flip 模型交换链。
- 设备建在源显示器所属显卡，跨显卡输出交给 DWM 合成。
- 镜像窗口无边框置顶、铺满目标显示器，并且不进入任务栏与 Alt+Tab。

**控制面板**

- Compose Multiplatform 面板跑在 WebView2 里，显示器 / 窗口枚举、启停、缩放、垂直同步与实时统计都在一个窗口完成。
- 面板与原生之间是 `postMessage` + JSON 桥；错误文案由原生层给出，Kotlin 只负责透传展示。
- 关闭面板缩到托盘；托盘菜单可显示面板、停止镜像或退出。
- 页头「打开终端」通过 keel `Messenger.post` 打开 `keel-plugin-console` 窗口，可查看日志或执行 Host 命令。

**打包与运行**

- `keelPlugin` 产出 `.keel`；`keelPlugin.executable` 产出内嵌 `keel-bundle.zip` 的单文件 exe。
- 单 exe 使用 Windows GUI 子系统（`console = false`），双击不弹控制台，日志走内置的 console 插件。
- `.keel` 与 `resources/` 运行期只在内存打开，只有原生 DLL 物化到 `.cache/<id>/<version>/`。
- 工作目录按 `KEEL_HOME` → exe 目录 → 用户缓存目录回退。

## 架构

### 分层

| 层 | 技术 | 位置 |
| --- | --- | --- |
| 采集 + 呈现核心 | C++ / D3D11（桌面复制 + WGC） | `native/` |
| UI 外壳 | C++ / Win32 + WebView2（窗口、托盘、JSON 桥） | `native/webview/` |
| 控制面板 | Compose Multiplatform（wasmJs） | `ui/` |
| keel 插件入口 | Kotlin/Native（物化资源、拉起外壳、转发终端请求） | `keel-plugin-klive/` |
| 共享模型 | Kotlin Multiplatform（jvm + wasmJs + mingwX64 + linuxX64，无 Compose） | `core/` |

`core` / `ui` / `keel-plugin-klive` 必须拆开：Compose 编译器插件作用于整个 Gradle 工程，工程里一旦声明 mingwX64 / linuxX64 目标就会因缺少 Compose Runtime 而编译失败。把模型放 `core`、把 Compose 放 `ui`，keel 插件才能只依赖 `core`。

### 渲染路径

```text
桌面复制 / WGC 帧
  → CopyResource 到自有纹理
  → D3D11 全屏四边形采样
  → flip 模型交换链 Present
  → 目标显示器
```

CPU 不参与像素搬运。目标显示器挂在另一块显卡时由 DWM 负责跨卡合成（本机已验证：RX 7900 XTX 上的 4K 主屏 → 核显上的 1080p 显示器，稳定 60fps）。

### 跨语言边界

原生层只暴露 [klive.h](native/include/klive.h) 与 [klive_webview.h](native/webview/klive_webview.h) 两份 C ABI。结构体布局、定长 UTF-8 字符串、返回码 + `klive_last_message()` 的错误模型都以头文件为唯一来源；Kotlin 侧只经 cinterop 与 JSON 桥接触原生，不持有 C++ 类型。

## 快速开始

### 环境要求

- Windows 10 1903+ / Windows 11（实测 Win11）。
- JDK 21。
- Visual Studio 2022，含「使用 C++ 的桌面开发」工作负载。
- CMake 3.20+。
- WebView2 Runtime。
- keel `0.1.0-SNAPSHOT` 已发布到本机 `~/.m2`（`keel-api` / `keel-host` / `keel-launcher` / `keel-plugin-console`）。

### 构建

```powershell
# 编译原生库 -> native/build/cmake/Release/{klive_capture,klive_webview}.dll
.\gradlew.bat :native:nativeBuild

# 打出插件包 -> keel-plugin-klive/build/keel/keel-plugin-klive-0.1.0.keel
.\gradlew.bat :keel-plugin-klive:keelPackage

# 打出单文件 exe -> keel-plugin-klive/build/keel/mingwX64/KigerLiveHelper.exe
.\gradlew.bat :keel-plugin-klive:keelExecutableMingwX64

# 用 keel Host 加载插件并打开控制面板（开发自检）
.\gradlew.bat :keel-plugin-klive:keelRun
```

### 运行

单 exe 内嵌控制面板、原生 DLL 与 `keel-plugin-console`，双击即可运行。首次运行会在 exe 同目录创建 `plugins/` 与 `.cache/`；目录不可写时自动回退到用户缓存目录。

只调面板布局时可以直接在浏览器里跑（Mock 引擎，不碰原生层）：

```powershell
.\gradlew.bat :ui:wasmJsBrowserDevelopmentRun
```

## 使用

1. 选择「镜像档位」：全屏镜像 / 跟随鼠标屏幕 / 鼠标下窗口 / 前台窗口 / 2× / 3× / 4× 缩放。
2. 全屏档下，左侧选择采集源（显示器或窗口，显示器可选桌面复制 / WGC），右侧选择目标显示器。
3. 按需切换「等比填充 / 拉伸铺满」与「垂直同步」，点「开始镜像」。
4. 镜像运行中，鼠标侧键可切档：后退键上一档、前进键下一档（只在镜像运行时拦截）。
5. 退出镜像：`Ctrl + Alt + Q`；镜像窗口拿到焦点时按 `Esc` 也可以。
6. 页头「打开终端」打开控制台窗口，可查看日志、执行 `help` / `plugins` 等 Host 命令。
7. 关闭面板只会缩到托盘；托盘菜单可以「显示面板 / 停止镜像 / 退出」。

档位与全屏档的源 / 目标显示器存在用户数据目录 `%LOCALAPPDATA%\klive\settings.json`，下次启动自动恢复。keel 的 `config/plugins/klive.yaml` 只作为镜像/迁移种子，不作为唯一来源。

## 构建任务

| 任务 | 说明 |
| --- | --- |
| `gradlew :native:nativeBuild` | 编译 `klive_capture.dll` 与 `klive_webview.dll` |
| `gradlew :native:nativeSmoke` | WebView2 外壳冒烟，8 秒后自动退出 |
| `gradlew :ui:wasmJsBrowserDevelopmentRun` | 浏览器里跑 wasm 面板（Mock 引擎） |
| `gradlew :keel-plugin-klive:keelPackage` | 打包 `.keel`（原生 DLL + wasm 面板） |
| `gradlew :keel-plugin-klive:keelExecutableMingwX64` | 产出内嵌 bundle 的单文件 exe |
| `gradlew :keel-plugin-klive:keelRun` | Host 加载插件，打开 WebView2 面板 |

改了任何代码至少跑一次 `keelRun`。只改 Kotlin 时可用 `:ui:compileKotlinWasmJs` 快速校验；改了 C ABI 结构体必须跑 `keelRun` 看枚举结果，编译通过不等于两边布局一致。

## 目录结构

```text
kigers-live-helper/
├── native/                        C++ 核心 + Win32/WebView2 外壳
│   ├── include/klive.h            C ABI（结构体布局 + 导出函数）
│   ├── src/                       枚举、镜像会话、渲染循环、FrameSource
│   └── webview/                   WebView2 外壳、托盘、JSON 桥
├── core/                          KMP 共享模型与 MirrorEngine 端口（无 Compose）
├── ui/                            Compose Multiplatform 控制面板（wasmJs / jvm）
├── keel-plugin-klive/             keel 插件：资源物化、面板生命周期、终端转发
├── doc/                           项目记忆与设计材料
└── gradle/libs.versions.toml      版本唯一来源
```

## 当前状态

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| C ABI：枚举 / 启停 / 统计 / 错误消息 | 可用 | 显示器、窗口、桌面复制与 WGC |
| 桌面复制后端 | 可用 | 4K@160 → 1080p@60 实测 60fps |
| WGC 显示器后端 | 可用 | 4K → 1080p 实测约 52fps |
| WGC 窗口后端 | 可用 | 可采前台窗口，静态画面帧率低属正常 |
| 缩放 / 垂直同步 / 帧率统计 | 可用 | 等比填充与拉伸铺满 |
| Compose 控制面板 | 可用 | wasmJs 跑在 WebView2，jvm 可本地调试 |
| keel 插件与 `.keel` 打包 | 可用 | `keelRun` 端到端跑通 |
| 单 exe（桌面应用模式） | 可用 | 内嵌插件 bundle，双击启动，不弹控制台 |
| 「打开终端」 | 可用 | 面板按钮 → WebView2 桥 → `Messenger.post` → console 插件 |
| 镜像档位 | 可用 | 全屏 / 跟随鼠标屏 / 鼠标下窗口 / 前台窗口 / 2× / 3× / 4× |
| 鼠标侧键切档 | 可用 | `WH_MOUSE_LL`，仅镜像运行时拦截 XBUTTON1/2 |
| 配置持久化 | 可用 | 档位与全屏档的源 / 目标屏存插件 YAML |
| 光标 / HDR / 多路镜像 | 未做 | 见「限制与已知缺口」 |

## 限制与已知缺口

- 只支持 Windows；桌面复制需要 Win8+，WGC 需要 Win10 1903+（实测 Win11）。
- 桌面复制对每台显示器独占：OBS 之类正在捕获同一显示器时会失败并提示，改用 WGC 即可共存；窗口采集只能走 WGC。
- 源显示器锁屏、切换分辨率、拔插显示器时复制会话可能失效；桌面复制会自动重建，失败则把错误交给 UI。
- 源画面是 HDR（FP16 桌面格式）时目前按 SDR 直出，颜色可能偏亮，尚未做色调映射。
- 桌面复制不带鼠标光标；WGC 打开了 `IsCursorCaptureEnabled`，两种后端的光标行为不一致（缩放档尤其明显）。
- 动态窗口档在窗口关闭 / 最小化时会丢源重试；全屏档的固定窗口源仍可能停帧或报错。
- 侧键依赖 `WH_MOUSE_LL`：厂商驱动把侧键改成键盘键、或反作弊拦截低级钩子时收不到 XBUTTON。
- 暂停快捷键、多路镜像、热插拔刷新都还没做。
- 单 exe 目前约 41MB（debug launcher + debug 插件 DLL + 两份 wasm），release launcher 与体积裁剪待上游。

## 进一步阅读

| 文档 | 内容 |
| --- | --- |
| [AGENTS.md](AGENTS.md) | 目录边界、代码风格、C ABI 与线程约定、验证与提交 |
| [doc/项目记忆.md](doc/项目记忆.md) | 长任务交接：已完成、实测数据、设计决策、易踩的坑 |
| [doc/keel插件化改造方案.md](doc/keel插件化改造方案.md) | 从 JVM 外壳迁移到 keel 插件的方案与取舍 |
| [keel](https://gitee.com/lolosia/keel) | 插件宿主、C ABI、`.keel` 打包与单 exe 的实现 |
