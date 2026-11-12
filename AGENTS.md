# AGENTS.md —— 项目风格与硬性约定

> 在本仓库动手前先读本文。它是**规范性**文档（约定，不是教程）：写代码、改结构、加依赖都按这里来。
> 与代码冲突时以代码为准，并顺手回来修本文。
> 当前进度与踩坑清单见 [doc/项目记忆.md](doc/项目记忆.md)。
>
> 面向对象：AI agent 与人类协作者一视同仁 —— 约定就是约定。

## 0. 项目定位

- **klive**：Windows 桌面屏幕镜像工具。把一台显示器（或一个窗口）的画面实时镜像到另一台显示器。
- 包根 **`moe.lolosia.klive`**，Gradle 工程名 `klive`，版本 `0.1.0`。
- 技术形态：**C++/D3D11 原生核心 + Kotlin/JVM 外壳**，二者只通过一份 C ABI 接触。
  - `native/`：CMake + MSVC，产出 `klive_capture.dll`；
  - `app/`：Kotlin/JVM + Compose Desktop 控制面板，用 JNA 调原生库；
  - `core/`：纯 Kotlin 共享模型（KMP，jvm + mingwX64 + linuxX64，**不含 Compose**）。
- 之所以拆 `core`：Compose 编译器插件作用于整个 Gradle 工程，工程里一旦有原生目标就会因缺
  Compose Runtime 而编译失败。把模型放 `core`，才能既保住桌面 UI，又让共享代码能编译到
  keel 的目标上。
- 长期方向：镜像/串流核心将来要能被 **keel 插件**（局域网 WebRTC / RTSP）复用，
  所以原生层只暴露 C ABI、不暴露 C++ 类型。

## 1. 目录与构建边界

```
native/     C++ 核心（独立 CMake 工程，不进 Gradle 的源码集）
core/       KMP 共享模型（无 Compose、无平台 API）
app/        Compose Desktop 外壳 + JNA 绑定 + 原生库构建任务
doc/        长任务记忆与设计材料
```

- 依赖方向**单向**：`app -> core`、`app -> native/src`（构建期 CMake，运行期 JNA）。
  `core` 不得依赖 `app`，也不得引用任何 `java.*` / Compose / JNA 类型。
- `native/` 不认识 Kotlin，`core/` 不认识 C++。跨层只走 [native/include/klive.h](native/include/klive.h)。
- 原生库的构建任务写在 [app/build.gradle.kts](app/build.gradle.kts)（`nativeConfigure` /
  `nativeBuild` / `copyNativeLib`），CMake 源码在 `native/`；不要另起一套构建入口。

## 2. 构建与验证

```powershell
# 编译原生库（输出 app/build/native/cmake/Release/klive_capture.dll）
.\gradlew.bat :app:nativeBuild

# 启动控制面板（自动先构建原生库）
.\gradlew.bat :app:run

# 不开窗，只枚举显示器与窗口，用于排查原生链路
.\gradlew.bat :app:probe

# 打包 MSI（DLL 作为资源打进 jar，运行时解压到临时目录再加载）
.\gradlew.bat :app:packageMsi
```

- **改了任何代码至少跑**：`.\gradlew.bat :app:probe`（它会加载 DLL、枚举显示器与窗口，
  JNA 结构体布局对不上时这里就会先炸）。
- 只改 Kotlin 时可用 `.\gradlew.bat :app:compileKotlin` 快速校验；**但 Kotlin 侧的
  `Klive*Struct` 改了必须跑 probe**，编译通过不等于 ABI 对齐。
- 本机要求：JDK 21、Visual Studio 2022（含 C++ 桌面工作负载）、CMake 3.20+。
- 没有自动化测试；验证靠 probe + 手动点「开始镜像」+ 帧率 / 画面尺寸读数。

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
     `ScaleMode` / `MirrorRequest` / `MirrorStats`）。这些类型体量都很小、总是一起用；
     某个类型长出行为或依赖时再单独拆文件。
   - `private` 顶层声明不受此限：单行常量、只服务本文件的辅助函数，就近放使用处。
5. **散装函数 / 常量放同包 `package.kt`**；Kotlin 侧目前没有，需要时按此约定新建。
6. **扩展函数的归属**：扩展外部库类型按库分域；扩展自研类型与类型同域；
   只服务单个文件的 `private` 扩展留在原文件。
7. **UI 状态提升到 `App()`**：Composable 只做展示与回调，不放业务状态机。
   跨帧可变状态用 `mutableStateOf` + `by remember`，不要在 Composable 里攒普通 `var`。
8. 展示用文案（中文）直接写在 Composable 里；原生层的错误文案在 C++ 里拼成 UTF-8
   （`klive_set_message`），Kotlin 只负责透传展示，不要两边各写一份。
9. CLI 自检（`--probe`）可以直接 `println`；这是本项目唯一的「日志」出口，
   不要为此引入日志框架。

## 4. 跨语言边界（C ABI）

[klive.h](native/include/klive.h) 是**唯一布局来源**：改 ABI 先改头文件，再同步
`internal.h` / 实现 / Kotlin 侧 JNA 结构体，顺序不要反。

- **结构体布局必须两边一致**，且要经得起 JNA 的默认对齐：
  - 不放 4 字节以下的字段（`bool` / `short` 一律用 `int32_t`）；
  - **总大小保持 8 的整数倍**，必要时用 `int32_t reserved` 补齐；
  - 宽字段（`int64_t` / `double`）放在结构体开头，避免中间出现隐式 padding。
  - 当前尺寸（对方改了必须同步）：`klive_monitor_desc` = 292、`klive_window_desc` = 376、
    `klive_mirror_config` = 32、`klive_mirror_stats` = 32。
- 字符串字段是**定长 `char[]` + UTF-8**，不是指针；Kotlin 侧用 `ByteArray(n)` + 去尾部 0 解码。
- 错误一律走「返回码 + `klive_last_message()`」：返回码给程序判断，message 给人看。
  新增错误码要同时补 `MirrorEngine.errorText()`。
- JNA 方法名与导出符号**必须完全一致**，所以 `KliveLibrary` 里的方法不能是 `internal`
  （Kotlin 会给 internal 成员加名字修饰）。
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
- 全屏四边形的固定要求（回退就会黑屏）：`D3D11_CULL_NONE` 光栅化状态 + 显式 `viewport` +
  flip 模型交换链（`DXGI_SWAP_EFFECT_FLIP_DISCARD`）。
- 采集失败要有可读原因：`klive_set_dxgi_error` 已覆盖常见的 `DXGI_ERROR_*`，
  新增失败路径请复用，不要只回一个码。

## 7. 验证与提交

- 提交前自检源码没被 `.gitignore` 误吞（`build/`、`.gradle/`、`*.log` 都在忽略列表里）。
- **凭据、账号、服务器地址一律不入库**（含本文档与 `doc/`）；用占位符。
- 不要提交 `app/build/**` 下的原生产物；DLL 由 `nativeBuild` 现编。
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
native/src/frame_source.h/.cpp       FrameSource 抽象、FrameCopy 中转纹理、DXGI 错误文案
native/src/klive_duplication.cpp     桌面复制后端
native/src/klive_wgc.cpp             WGC 后端（C++/WinRT，显示器 + 窗口）
core/src/commonMain/.../model/MonitorInfo.kt   共享模型（见 §3.4 例外）
app/src/main/kotlin/.../Main.kt                 入口；`--probe` 自检
app/src/main/kotlin/.../capture/KliveLibrary.kt JNA 绑定 + 结构体 + DLL 加载
app/src/main/kotlin/.../capture/MirrorEngine.kt 原生会话的薄封装（UI 只认这个）
app/src/main/kotlin/.../ui/App.kt              控制面板（采集源 / 目标屏 / 选项 / 统计）
app/src/main/kotlin/.../ui/theme/Theme.kt      深色控制台配色
```

## 10. 文档地图

| 文档 | 内容 |
| --- | --- |
| [README.md](README.md) | 面向使用者的简介、架构、构建与使用步骤、限制 |
| [doc/项目记忆.md](doc/项目记忆.md) | 长任务交接：已完成、实测数据、设计决策、缺口、易踩的坑 |
