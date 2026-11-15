# AGENTS.md —— 项目风格与硬性约定

> 在本仓库动手前先读本文。它是**规范性**文档（约定，不是教程）：写代码、改结构、加依赖都按这里来。
> 与代码冲突时以代码为准，并顺手回来修本文。
> 当前进度与踩坑清单见 [doc/项目记忆.md](doc/项目记忆.md)。
>
> 面向对象：AI agent 与人类协作者一视同仁 —— 约定就是约定。

## 0. 项目定位

- **Kiger's Live Helper（Kiger直播助手）**：Windows 桌面屏幕镜像工具。把一台显示器（或一个窗口）的画面实时镜像到另一台显示器。
- 包根 **`moe.lolosia.klive`**，Gradle 工程名 `klive`（`klive` 只是包名缩写，不是产品名），版本 `0.1.0`。
- 技术形态：**C++/D3D11 原生核心 + keel 插件 + Compose wasmJs 面板**，Kotlin 侧只经 C ABI 接触原生。
  - `native/`：CMake + MSVC，产出 `klive_capture.dll`（采集/呈现）与 `klive_webview.dll`（WebView2 外壳）；
  - `keel-plugin-klive/`：K/N 薄插件，把 DLL 与 wasm 面板物化后拉起外壳；
  - `ui/`：Compose Multiplatform 控制面板（wasmJs 供 WebView2，jvm 供本地调试）；
  - `core/`：纯 Kotlin 共享模型与 `MirrorEngine` 端口（KMP，jvm + wasmJs + mingwX64 + linuxX64，**不含 Compose**）。
- 之所以拆 `core` / `ui`：Compose 编译器插件作用于整个 Gradle 工程，工程里一旦有原生目标就会因缺
  Compose Runtime 而编译失败。把模型放 `core`、把 Compose 放 `ui`，才能让 keel 插件只依赖 `core`。
- 长期方向：镜像/串流核心将来要能被 **keel 插件**（局域网 WebRTC / RTSP）复用，
  所以原生层只暴露 C ABI、不暴露 C++ 类型。

## 1. 目录与构建边界

```
native/                 C++ 核心 + WebView2 外壳（独立 CMake 工程，不进 Gradle 的源码集）
core/                   KMP 共享模型与 MirrorEngine 端口（无 Compose、无平台 API）
ui/                     Compose Multiplatform 控制面板（jvm + wasmJs），桌面与 WebView2 共用
keel-plugin-klive/      keel 插件（mingwX64），物化 DLL 与 wasm 后拉起 WebView2
doc/                    长任务记忆与设计材料
```

- 依赖方向**单向**：`ui -> core`、`keel-plugin-klive -> keel-api`（运行期加载原生 DLL）。
  `core` 不得依赖 `ui` / `keel-plugin-klive`，也不得引用任何 `java.*` / Compose / JNA 类型。
- `native/` 不认识 Kotlin，`core/` 不认识 C++。跨层只走 [native/include/klive.h](native/include/klive.h)
  与 [native/webview/klive_webview.h](native/webview/klive_webview.h)。
- 原生构建任务在 [native/build.gradle.kts](native/build.gradle.kts)（`nativeConfigure` /
  `nativeBuild` / `nativeSmoke`），CMake 源码在 `native/`；不要另起一套构建入口。

## 2. 构建与验证

```powershell
# 编译原生库（输出 native/build/cmake/Release/klive_capture.dll + klive_webview.dll）
.\gradlew.bat :native:nativeBuild

# 打出 .keel（原生 DLL + wasm 面板）
.\gradlew.bat :keel-plugin-klive:keelPackage

# 打出内嵌 bundle 的单文件 exe（webview 与 wasm 面板都在里面）
.\gradlew.bat :keel-plugin-klive:keelExecutableMingwX64

# 用 keel Host 加载插件并打开 WebView2 面板（端到端自检）
.\gradlew.bat :keel-plugin-klive:keelRun

# WebView2 外壳冒烟：打开 smoke 页面，8 秒后自动退出
.\gradlew.bat :native:nativeSmoke

# 只在浏览器里调 wasm 面板（Mock 引擎，不碰原生层）
.\gradlew.bat :ui:wasmJsBrowserDevelopmentRun
```

- **改了任何代码至少跑**：`.\gradlew.bat :keel-plugin-klive:keelRun`（物化 DLL、打开面板、
  枚举显示器与窗口；ABI 或桥协议对不上时这里会先炸）。
- 只改 Kotlin 时可用 `.\gradlew.bat :ui:compileKotlinWasmJs` 快速校验；**改了 C ABI 结构体必须
  跑 `keelRun` 看枚举结果**，编译通过不等于两边布局一致。
- 本机要求：JDK 21、Visual Studio 2022（含 C++ 桌面工作负载）、CMake 3.20+、WebView2 Runtime。
- 单 exe 由 `keelPlugin.executable` 产出：keel launcher + 内嵌 `keel-bundle.zip`，
  `.keel` 与 `resources/` 运行期只在内存打开，只有原生 DLL 物化到 `.cache/<id>/<version>/`。
  发布时只需一个 exe；工作目录按 `KEEL_HOME` → exe 目录 → 用户缓存目录回退。
- 单 exe 用 `console = false`（Windows GUI 子系统），日志不写 stdout；需要看日志 / 敲命令时
  由面板「打开终端」让内嵌的 `keel-plugin-console` 开窗口，或启动前设 `KEEL_CONSOLE=1`。
- 没有自动化测试；验证靠 `nativeSmoke` / `keelRun` + 手动点「开始镜像」+ 帧率 / 画面尺寸读数。

## 3. 代码风格

沿用 lolosia 的 Kotlin 约定，**新代码一律照此写**：

1. **不写 `public`**：Kotlin 默认即 public；只有 `internal` / `private` 显式标注。
2. **import 用通配**：`import moe.lolosia.klive.model.*`，不逐条展开；同包不写 import。
   例外：`androidx.compose.*` 保持逐条导入（同名类型多，通配容易撞名）。
   现状：早期文件是逐条导入，改动时按需顺手收敛，不必为改 import 单独动文件。
3. **注释只写正经 KDoc**：用途 / 参数 / 不变量 / 真实约束。
   - 不写「改动历史」「迁移自哪里」「参照方案第几节」这类叙述 —— 那是 git 的事。
   - 注释里**不要出现 `/*`**（Kotlin 块注释可嵌套，`/db/*` 会把后面的代码吞掉）。
   - C++ 侧同理：解释「为什么必须这样」（如 SRWLOCK、CULL_NONE），不解释「改了什么」。
4. **一个 `.kt` 只放一个顶层类型**（class / object / interface / enum），嵌套类随宿主保留。
   - 当前唯一例外：[MonitorInfo.kt](core/src/commonMain/kotlin/moe/lolosia/klive/model/MonitorInfo.kt)
     集中放共享模型（`MonitorInfo` / `WindowInfo` / `CaptureBackend` / `CaptureSourceKind` /
     `ScaleMode` / `MirrorMode` / `MirrorRequest` / `MirrorSettings` / `MirrorStats`）。
     这些类型体量都很小、总是一起用；
     某个类型长出行为或依赖时再单独拆文件。
   - `private` 顶层声明不受此限：单行常量、只服务本文件的辅助函数，就近放使用处。
5. **散装函数 / 常量放同包 `package.kt`**；Kotlin 侧目前没有，需要时按此约定新建。
6. **扩展函数的归属**：扩展外部库类型按库分域；扩展自研类型与类型同域；
   只服务单个文件的 `private` 扩展留在原文件。
7. **UI 状态提升到 `App()`**：Composable 只做展示与回调，不放业务状态机。
   跨帧可变状态用 `mutableStateOf` + `by remember`，不要在 Composable 里攒普通 `var`。
8. 展示用文案（中文）直接写在 Composable 里；原生层的错误文案在 C++ 里拼成 UTF-8
   （`klive_set_message`），Kotlin 只负责透传展示，不要两边各写一份。
9. CLI 自检（`nativeSmoke`、`klive_webview_probe`）可以直接 `printf` / `println`；
   keel 插件里的运行日志走 `Log.info(...)`，不要为此引入日志框架。

## 4. 跨语言边界（C ABI）

[klive.h](native/include/klive.h) 是**唯一布局来源**：改 ABI 先改头文件，再同步
`internal.h` / 实现 / `klive_webview.cpp` 里的 JSON 桥，顺序不要反。

- **结构体布局必须两边一致**，且要经得起 JNA 的默认对齐：
  - 不放 4 字节以下的字段（`bool` / `short` 一律用 `int32_t`）；
  - **总大小保持 8 的整数倍**，必要时用 `int32_t reserved` 补齐；
  - 宽字段（`int64_t` / `double`）放在结构体开头，避免中间出现隐式 padding。
  - 当前尺寸（对方改了必须同步）：`klive_monitor_desc` = 296、`klive_window_desc` = 376、
    `klive_mirror_config` = 40、`klive_mirror_stats` = 40。
  - `klive_webview_config` 的 `data_dir`（由插件传 `ctx.dataDir`）与尾部
    `settings_json` / `save_settings` / `save_settings_user` 都是破坏性变更，
    改完必须重编原生 DLL、keel 插件与 `klive_webview_smoke`。
- 目录规范跟随 keel，不自己找用户目录：镜像设置写 keel 插件配置
  `config/plugins/klive.yaml`（`ctx.config`），原生只经 `settings_json` / `save_settings`
  与插件交换；WebView2 profile 放 `ctx.dataDir\WebView2`。单 exe 在 Gradle 里设
  `dataMode = KeelDataMode.USERDATA`、`appId = "klive"`，数据根落在 `%LOCALAPPDATA%\klive`。
- 字符串字段是**定长 `char[]` + UTF-8**，不是指针；桥里转成普通字符串。
- 错误一律走「返回码 + `klive_last_message()`」：返回码给程序判断，message 给人看。
  新增错误码要同时补桥里的错误文案。
- 镜像档位（`KLIVE_MODE_*`）由原生采集层实时解析源；`mode != KLIVE_MODE_FULLSCREEN`
  时 `source_index` / `window_handle` 只是解析失败时的回退值，不能当作当前源。
- 64 位句柄（`handle`、`adapter_luid_*`）在 JSON 桥里用字符串传，避免 JS 数字精度问题。
- 原生层只暴露 C 函数与 POD；C++ 类型（`ComPtr`、`winrt::*`）一律留在 `.cpp` / 内部头里。

## 5. 线程与并发

- 所有采集与渲染都在**原生 worker 线程**上；`klive_start_mirror` 只负责校验参数、拉起线程，
  立即返回，UI 线程不会被采集阻塞。
- 会话状态（`running` / `stop` / 统计量）是 `std::atomic`；生命周期用 `SRWLOCK`。
- worker 线程可能自行退出（出错 / 用户按 Esc），所以 start 与 stop **都要 `join`**：
  `std::thread` 在函数体结束后仍是 joinable，漏 join 会在退出时 abort。
- WGC 需要 COM apartment：worker 线程进入采集前调 `winrt::init_apartment(multi_threaded)`，
  失败（已初始化）可忽略，不要重复初始化。

## 6. 采集与呈现

- 采集抽象在 [frame_source.h](native/src/frame_source.h)：`FrameSource::grab()` 返回
  「本轮有没有新帧」，渲染循环不感知后端差异。新增后端实现这个接口即可，不要改渲染循环。
- 两种后端的选择由 `capture_backend` / `source_kind` 决定：
  - `KLIVE_CAPTURE_DUPLICATION`：桌面复制，性能最好，**每台显示器独占**；
  - `KLIVE_CAPTURE_WGC`：Windows Graphics Capture，可多程序共存，**窗口采集的唯一选择**。
  - 窗口 + 桌面复制是非法组合，原生层直接拒绝。
- **帧纹理必须中转**：DXGI / WGC 的帧纹理生命期只有一帧，统一 `CopyResource` 到自己的
  纹理再画（`FrameCopy`）。不要在渲染路径上直接持有帧池纹理。
- 设备建在**源显示器所属显卡**上；跨显卡呈现交给 DWM 合成（已验证可用）。
  窗口采集时设备建在目标显示器所在显卡上。
- 动态档位（跟随鼠标 / 窗口 / 缩放）每 250ms 重新解析源：同卡只换 `FrameSource`，
  跨卡才重建 device / swapchain，镜像窗口本身不重建。
- 换卡重建时必须**先释放旧视图 / 交换链，再 `ClearState` + `Flush`**，最后才
  `CreateSwapChainForHwnd`：一个 HWND 同时只能挂一个 flip 交换链，D3D11 的延迟销毁
  会让顺序反过来的重建报 `E_ACCESSDENIED`。
- **HDR 输出**：用 `IDXGIOutput6::GetDesc1().ColorSpace` 判断，HDR 屏一律走 WGC 的
  `R16G16B16A16_FLOAT` 帧池（scRGB 线性，1.0 = 80 nits），按 `GET_SDR_WHITE_LEVEL`
  归一化 SDR 白后再查色调曲线 + sRGB 编码。不要用 BGRA8 帧池（系统压出来的高光会
  裁白），也不要用桌面复制（HDR 下表面颜色编码不可靠）。
- **HDR 色调曲线**：`klive_set_tone_curve` 接收 65 点线性 LUT（输入 0..2，输出 0..1），
  原生层存进 `Texture1D`，着色器按线性采样；面板在 HDR 显示器行给出「···」入口，
  用 6 个控制点拖曲线，LUT 由 `core` 的 `ToneCurve.lut()` 生成。曲线变化时渲染循环
  主动补一帧，静态画面也能立刻看到效果。
- 全屏四边形的固定要求（回退就会黑屏）：`D3D11_CULL_NONE` 光栅化状态 + 显式 `viewport` +
  flip 模型交换链（`DXGI_SWAP_EFFECT_FLIP_DISCARD`）。
- 采集失败要有可读原因：`klive_set_dxgi_error` 已覆盖常见的 `DXGI_ERROR_*`，
  新增失败路径请复用，不要只回一个码。

## 7. 验证与提交

- 提交前自检源码没被 `.gitignore` 误吞（`build/`、`.gradle/`、`*.log` 都在忽略列表里）。
- **凭据、账号、服务器地址一律不入库**（含本文档与 `doc/`）；用占位符。
- 不要提交 `native/build/**` 与 `keel-plugin-klive/build/**` 下的产物；DLL 与 `.keel` 由任务现编。
- 一次提交只做一件事；信息用中文，主题行写「做了什么」，正文写「为什么」。

## 8. 文档

- 本文件是规范，[doc/项目记忆.md](doc/项目记忆.md) 是状态（进度 / 坑 / 下一步）。
  两者都不要写成教程。
- 踩过的坑写进 `doc/项目记忆.md` 的「易踩的坑」，并说明**症状**，方便下次直接搜到。
- 设计类材料（ABI 变更、性能实测、竞品对比）放 `doc/` 下单独文件，命名用中文短名。

## 9. 文件地图

```
native/include/klive.h               C ABI（结构体布局 + 导出函数，唯一布局来源）
native/src/internal.h                内部共享：显示器枚举结果、会话对象、SRWLOCK 包装
native/src/klive_exports.cpp         导出函数实现（枚举 / 启动 / 停止 / 统计 / 消息）
native/src/klive_monitor.cpp         显示器与窗口枚举、错误消息缓冲
native/src/klive_mirror.cpp          镜像窗口 + 渲染循环 + 缩放 + Present + 帧率统计
native/src/klive_osd.h/.cpp          镜像左上角 OSD：GDI 文字位图 → 预乘 alpha 贴图叠加
native/src/frame_source.h/.cpp       FrameSource 抽象、FrameCopy 中转纹理、DXGI 错误文案
native/src/klive_duplication.cpp     桌面复制后端
native/src/klive_wgc.cpp             WGC 后端（C++/WinRT，显示器 + 窗口）
core/src/commonMain/.../model/MonitorInfo.kt          共享模型（见 §3.4 例外）
core/src/commonMain/.../capture/MirrorEngine.kt       UI 依赖的镜像端口
ui/src/commonMain/.../ui/App.kt                       控制面板（采集源 / 目标屏 / 选项 / 统计）
ui/src/commonMain/.../ui/theme/Theme.kt               深色控制台配色 + 字体注入
ui/src/wasmJsMain/.../ui/main.kt                      wasm 入口（ComposeViewport）
ui/src/wasmJsMain/.../ui/bridge/Bridge.kt             postMessage 请求 / 回包
ui/src/wasmJsMain/.../ui/engine/WasmMirrorEngine.kt   桥实现的 MirrorEngine
native/webview/klive_webview.h                        WebView2 外壳 C ABI
native/webview/klive_webview.cpp                      窗口 / 托盘 / 资源 / JSON 桥
keel-plugin-klive/src/commonMain/resources/plugin.json  插件清单（id / 显示名 / 库路径）
keel-plugin-klive/src/nativeMain/.../KlivePlugin.kt   插件生命周期：物化 + 启动外壳 + 配置读写
keel-plugin-klive/src/nativeMain/.../MirrorSettings.kt  持久化的档位 / 显示器身份
keel-plugin-klive/src/nativeMain/.../MirrorSettingsBridge.kt 原生回调 → 事件泵投递口
```

## 10. 文档地图

| 文档 | 内容 |
| --- | --- |
| [README.md](README.md) | 面向使用者的简介、架构、构建与使用步骤、限制 |
| [doc/项目记忆.md](doc/项目记忆.md) | 长任务交接：已完成、实测数据、设计决策、缺口、易踩的坑 |
