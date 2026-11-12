# keel 插件化改造方案（去 JVM + wasmJs UI + WebView2）

> 目标：klive 从「JVM + Compose Desktop」迁移为「keel 插件 + Compose wasmJs UI + WebView2」，
> 以 `klive-0.1.0.keel` 交付；C++ 采集核心保持不变。
> 前置：keel 已完成 `cacheDir` / `materialize` / `classpath`（commit `8be840a`），并重新发布 `mavenLocal`。
> 状态：已实施（Phase 0–4 完成：`app` / JNA / MSI 已删除，交付 `.keel`）。

---

## 0. 结论

采用「薄插件 + 厚原生壳」：

```text
Compose wasmJs UI（Chromium / WebView2 内）
        │ postMessage + JSON
        ▼
klive_webview.dll（MSVC，Win32 + WebView2 + 桥 + 直接调用 C ABI）
        │ klive.h
        ▼
klive_capture.dll（MSVC，D3D11 / WGC，保持不变）
```

K/N 插件只做三件事：

1. `ctx.materialize("native")` / `ctx.materialize("web")` 把 `.keel` 内的文件变成真实路径；
2. 按绝对路径加载 `klive_webview.dll`，把两个目录传给它；
3. 在 `onDisable` 里停掉 WebView2 壳。

这样做的理由：

- WebView2 的 COM 回调、窗口、消息循环全在 MSVC/C++ 侧，**不引入 K/N 的 COM 互操作**；
- 不把 MSVC 的 C++ 静态库链进 K/N（MinGW/MSVC 混用不可靠）；
- 采集核心继续走稳定的 C ABI，Kotlin 侧完全不需要 JNA 或 cinterop；
- 以后如果要把 UI 壳复用到别的插件，`klive_webview.dll` 的 C ABI 仍然可用。

不采用的方案：

- K/N 直接用 `platform.windows` + COM 写 WebView2：回调实现和引用计数太容易出错；
- 把 `klive_capture` / `klive_webview` 静态并进插件 dylib：MSVC 的 C++ 静态库无法可靠链进 K/N 的 mingwX64；
- 保留 JVM 只把 UI 换成 Web：没有达到去 JVM 的目标，还多一层进程。

---

## 1. keel 侧已确认的能力

### 1.1 可用 API

keel commit `8be840a`（`feat: 插件资源目录与资源物化`）已实现：

```kotlin
interface PluginContext {
    /** 每次加载独占的可写目录：<cacheRoot>/<id>/<version>/<loadSeq>/ */
    val cacheDir: String

    /** 把 .keel 内资源物化到 cacheDir，返回绝对路径；支持文件与目录前缀 */
    fun materialize(path: String): String

    /** 自身 .keel 优先、回落 Host 的只读资源视图 */
    val classpath: Classpath
}
```

约束（来自 keel 的 KDoc 与 `AGENTS.md`）：

- `cacheDir`、`materialize`、`classpath` 都是**反向请求，只能在 Worker 线程调用**；
- `materialize` 返回的路径可以交给其它线程使用，但下次加载后旧路径指向旧目录；
- `cacheDir` 每次加载/启用都新建，旧目录由 Host 下次启动时回收；
- 物化拒绝 `..`、前导分隔符、盘符；同名同大小不重写；
- `.keel` 的 `resources/` 不需要解包即可通过 `classpath` 读，但二进制需要 `materialize`。

### 1.2 打包与加载

- `keel-plugin-gradle-plugin` 会把 `src/commonMain/resources`、`src/nativeMain/resources`、
  `keelPlugin { resources(...) }` 全部放进 `.keel` 的 `resources/`；
- `.keel` 结构：`plugin.json` + `lib/<target>/<dylib>` + `resources/...`；
- `HostPlugins.start()` 在 `startPlugin()` 之前调用 `PluginResources.beginLoad()`，
  所以 `cacheDir` 在 `onLoad` 之前就已就绪；
- `cacheRoot` 不可写时回退 `%LOCALAPPDATA%\keel\cache`。

### 1.3 发布状态（已确认）

`~/.m2` 的**内部文件**时间戳已是 **2026-11-12 11:28–11:29**（不是文件夹时间），并且已核对内容：

- `keel-api-...-sources.jar` 含 `PluginContext.kt` / `MaterializeRequest.kt` / `CacheDirReply.kt`；
- `keel-host-...-sources.jar` 含 `PluginResources.kt`；
- `keel-host-dev-mingwX64-...zip` 里的 `keel-host.exe` 能搜到 `PluginResources` / `materialize`。

所以 **mavenLocal 已经是新版本，Phase 0 不需要再发布**。只有 Gradle 仍缓存旧快照时，
才需要清一次缓存或加 `--refresh-dependencies`：

```powershell
Remove-Item -Recurse -Force "$env:USERPROFILE\.gradle\caches\modules-2\files-2.1\moe.lolosia.keel"
```

如果临时需要改用本地 Host 二进制，可以在 `keel-plugin-klive` 里用：

```kotlin
keelPlugin {
    runHostExecutable = "../keel/keel-host/build/bin/mingwX64/debugExecutable/keel-host.exe"
}
```

---

## 2. 目标工程结构

```text
kigers-live-helper/
  core/                       共享模型 + 平台无关接口（wasmJs / jvm / mingwX64）
    src/commonMain/.../model/MonitorInfo.kt
    src/commonMain/.../capture/MirrorEngine.kt      ← 新：UI 依赖的端口
  ui/                         Compose Multiplatform 控制面板（jvm + wasmJs）
    src/commonMain/.../ui/App.kt                     ← 从 app 搬来
    src/commonMain/.../ui/theme/Theme.kt
    src/wasmJsMain/.../Main.kt                       ← ComposeViewport 入口
    src/wasmJsMain/.../WasmMirrorEngine.kt
    src/wasmJsMain/.../Bridge.kt / Dto.kt
    src/wasmJsMain/resources/bridge.js
  keel-plugin-klive/          K/N 薄插件，产出 .keel
    src/commonMain/resources/plugin.json
    src/nativeMain/.../KlivePlugin.kt
    src/nativeMain/.../WebViewShell.kt
    build.gradle.kts
  native/                     C++ / MSVC（CMake，不进 Gradle 源码集）
    include/klive.h                                  ← 不变
    src/...                                          ← 采集核心不变
    webview/klive_webview.h
    webview/klive_webview.cpp
    webview/third_party/...                          ← WebView2 SDK / JSON
    build.gradle.kts                                 ← 新：只放 CMake 任务
  app/                        旧 JVM 外壳，Phase 4 已删除
  doc/
```

依赖方向：

```text
core ← ui
core ← app（迁移期，JVM 实现 MirrorEngine）
core ← keel-plugin-klive（只用到生命周期，不依赖 ui）
native → 无 Gradle 依赖，只产出 DLL
keel-plugin-klive → 打包 native 的 DLL + ui 的 wasm 产物
```

`core` 仍然不碰 Compose、JNA、平台 API；`MirrorEngine` 接口只放 `suspend` 函数和共享模型。

---

## 3. 关键设计

### 3.1 UI：wasmJs + 共享 Compose

`app` 里的 [App.kt](../app/src/main/kotlin/moe/lolosia/klive/ui/App.kt) 和
[Theme.kt](../app/src/main/kotlin/moe/lolosia/klive/ui/theme/Theme.kt) 只用到
`foundation` / `material3` / `runtime` / `kotlin.math` / `kotlinx.coroutines`，
没有 AWT/Swing 类型，可以原样放进 `ui/commonMain`。

需要改的只有 `MirrorEngine` 的调用方式：

```kotlin
// core/src/commonMain/.../capture/MirrorEngine.kt
interface MirrorEngine {
    suspend fun monitors(): List<MonitorInfo>
    suspend fun windows(): List<WindowInfo>
    suspend fun start(request: MirrorRequest): String?
    suspend fun stop()
    suspend fun stats(): MirrorStats
    suspend fun lastMessage(): String?
    fun shutdown()
}
```

- JVM 实现：把现有 [MirrorEngine.kt](../app/src/main/kotlin/moe/lolosia/klive/capture/MirrorEngine.kt)
  的 JNA 逻辑原样搬进 `app`，实现这个接口；枚举走 `Dispatchers.IO`，其余直接返回。
- wasm 实现：`WasmMirrorEngine` 通过 `Bridge` 发 JSON，`suspendCancellableCoroutine` 等回包。
- `App.kt` 里 `fun refresh()` 改 `suspend fun refresh()`；按钮回调用
  `rememberCoroutineScope().launch { ... }`；`LaunchedEffect(Unit) { refresh() }` 不变。
- 统计轮询 `while (running) { delay(300); stats = engine.stats() }` 不变。

wasm 入口（Compose Multiplatform 1.11 使用 `ComposeViewport`）：

```kotlin
// ui/src/wasmJsMain/.../Main.kt
@OptIn(ExperimentalComposeUiApi::class)
fun main() {
    val engine = if (Bridge.available()) WasmMirrorEngine() else MockMirrorEngine()
    ComposeViewport(document.body) {
        KliveTheme { App(engine) }
    }
}
```

`index.html` 的样式要求：`html, body, #compose { margin: 0; height: 100%; background: #0E1014; }`，
避免 canvas 不铺满。Compose 1.11 的 web 产物是 ES module，`index.html` 直接加载生成的 `.js` 即可。

### 3.2 桥协议

WebView2 用 `postMessage`，不用 `AddHostObjectToScript`（后者要 IDispatch，成本高）。

**JS → 原生**

```json
{"id": 12, "method": "enumerateMonitors", "params": {}}
```

方法：

| method | params | result |
| --- | --- | --- |
| `enumerateMonitors` | `{}` | `MonitorDto[]` |
| `enumerateWindows` | `{}` | `WindowDto[]` |
| `startMirror` | `MirrorConfigDto` | `null` / `error` |
| `stopMirror` | `{}` | `null` |
| `getStats` | `{}` | `StatsDto` |
| `lastMessage` | `{}` | `string` |

**原生 → JS**

```json
{"id": 12, "ok": true, "result": [...]}
{"id": 12, "ok": false, "error": {"code": -6, "message": "桌面复制失败：..."}}
```

要求：

- `handle`、`adapterLuidLow/High` 这类 64 位值**用字符串传**，避免 JS number 精度问题；
- 错误文案直接用 `klive_last_message()` 的 UTF-8，不在 Kotlin 里再写一份；
- `startMirror` 的 `captureBackend` / `sourceKind` / `scaleMode` 与 `klive.h` 的枚举值一致；
- 请求 id 由 wasm 侧生成；超时 5s 后以错误回包，避免 UI 永久卡住。

`bridge.js`（放在 wasm 产物目录，`index.html` 里先加载）：

```js
window.klive = {
  available: () => !!(window.chrome && window.chrome.webview),
  post: (msg) => window.chrome.webview.postMessage(msg),
  listen: (cb) => window.chrome.webview.addEventListener('message', (e) => cb(e.data)),
};
```

Kotlin/Wasm 侧用 `@JsFun("() => window.klive")` 拿到对象；浏览器开发模式没有
`window.chrome.webview`，`available()` 为 false，自动切到 `MockMirrorEngine`。

### 3.3 WebView2 壳（`klive_webview.dll`）

C ABI（`native/webview/klive_webview.h`）：

```c
typedef struct klive_webview_config {
    const char* native_dir;   /* UTF-8，含 klive_capture.dll / klive_webview.dll */
    const char* web_dir;      /* UTF-8，含 index.html */
    int32_t width;
    int32_t height;
    int32_t min_width;
    int32_t min_height;
    int32_t enable_devtools;
    int32_t reserved;
} klive_webview_config;

int32_t klive_webview_start(const klive_webview_config* config);
int32_t klive_webview_stop(void);
int32_t klive_webview_is_running(void);
const char* klive_webview_last_error(void);
const char* klive_webview_probe(const char* native_dir);  /* 可选：枚举结果，供控制台自检 */
```

行为：

- `start` 起一条**独立 UI 线程**，创建 Win32 顶层窗口 + WebView2，然后立即返回；
- UI 线程里先 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`，再建窗口；
- `LoadLibraryW("<native_dir>\\klive_capture.dll")`，用 `GetProcAddress` 解析
  `klive_initialize` / `klive_enumerate_monitors` / `klive_enumerate_windows` /
  `klive_start_mirror` / `klive_stop_mirror` / `klive_get_stats` / `klive_last_message`；
- `SetVirtualHostNameToFolderMapping(L"klive.local", web_dir, DENY_CORS)`，
  `Navigate(L"https://klive.local/index.html")`；
- 用 `WebResourceRequested` 对 `.wasm` / `.js` / `.mjs` 显式补 `Content-Type`
  （虚拟主机映射依赖系统 MIME，`.wasm` 在某些机器上会是 `application/octet-stream`）；
- `WebMessageReceived` 收 JSON，分派到采集 DLL，把结果 `PostWebMessageAsJson` 回页面；
- 关闭按钮：`WM_CLOSE` 只 `ShowWindow(SW_HIDE)`，不销毁窗口、不停止镜像；
- 托盘图标：`Shell_NotifyIcon`（`native/webview/klive.ico` + `.rc`），双击恢复窗口，
  右键菜单 `显示面板` / `停止镜像` / `退出`；
- 托盘退出：`klive_stop_mirror()` → `NIM_DELETE` → `DestroyWindow` → UI 线程退出 → `ExitProcess(0)`；
- `klive_webview_stop`：向 UI 线程投 `WM_CLOSE`/自定义消息，等待 join，带 3s 超时；
- WebView2 Runtime 缺失：`GetAvailableCoreWebView2BrowserVersionString` 失败时
  弹 MessageBox 给 Evergreen Bootstrapper 链接，并返回 `-3`；
- 用户数据目录：`%LOCALAPPDATA%\klive\WebView2`（持久化），不要用 `cacheDir`；
- 只允许 `https://klive.local/`，`NavigationStarting` 拦截外部导航；
- Release 构建关 DevTools、右键菜单、状态栏、缩放控件；Debug 打开 DevTools。

错误码建议：

| code | 含义 |
| --- | --- |
| 0 | 成功 |
| -1 | 参数无效 |
| -2 | `klive_capture.dll` 加载失败 |
| -3 | WebView2 Runtime 缺失 |
| -4 | WebView2 初始化失败 |
| -5 | 窗口创建失败 |
| -6 | 已经在运行 |
| -7 | 当前没有运行 |

JSON 用 `nlohmann/json` 单头文件（MIT），放在 `native/webview/third_party/`。

**WebView2 loader 选定：官方 `WebView2Loader.dll`**（NuGet 固定版本），
连同 `klive_capture.dll` / `klive_webview.dll` 一起物化到 `resources/native/`。

理由：`.keel` 本来就要带两个 DLL，多一个 loader 没有额外负担；官方 loader 与
WebView2 Runtime 的版本匹配最稳。`jchv/OpenWebView2Loader`（ISC）留作以后
「少一个 DLL / 单文件化」的优化项，接入前记录来源、commit 与许可。

### 3.4 插件生命周期与线程

```kotlin
@KeelPluginMain
class KlivePlugin : Plugin {
    private lateinit var ctx: PluginContext
    private var shell: NativeLibrary? = null

    override fun onLoad(context: PluginContext) {
        ctx = context
    }

    override fun onEnable() {
        // Worker 线程：materialize 是反向请求，必须在这里做
        val nativeDir = ctx.materialize("native")
        val webDir = ctx.materialize("web")
        val shell = loadShell("$nativeDir/klive_webview.dll")
        val rc = shell.start(nativeDir, webDir, width = 1120, height = 760, minWidth = 920, minHeight = 640)
        check(rc == 0) { shell.lastError() ?: "WebView2 启动失败：$rc" }
        this.shell = shell
    }

    override fun onDisable() {
        shell?.stop()          // 带超时，不要无限等
        shell = null
    }
}
```

要点：

- `onLoad` / `onEnable` 都在 keel Worker 线程；`materialize` 只能在这里调；
- `klive_webview_start` 内部起线程后立即返回，**不要**在 Worker 线程跑消息循环；
- `onDisable` 里 `stop` 要带超时（3s），避免 keel 的 5s 停机等待被拖死；
- UI 线程只使用传进去的路径字符串，不访问 `ctx`；
- 插件不静态 import `klive_capture.dll`，避免 Windows 在加载插件 dylib 时的依赖搜索问题。

### 3.5 资源与缓存

`.keel` 内布局：

```text
plugin.json
lib/mingwX64/keel_plugin_klive.dll
resources/native/klive_capture.dll
resources/native/klive_webview.dll
resources/native/WebView2Loader.dll
resources/web/index.html
resources/web/bridge.js
resources/web/*.js / *.mjs / *.wasm
```

运行期：

```text
<cacheRoot>/klive/0.1.0/<loadSeq>/
  native/klive_capture.dll
  native/klive_webview.dll
  native/WebView2Loader.dll
  web/index.html ...
```

- `cacheDir` 由 keel 每次加载新建，重载不会覆盖已加载的 DLL；
- 旧 `<loadSeq>` 目录由 Host 下次启动回收；
- `cacheRoot` 默认 `<pluginsDir>/.cache`，只读时回退 `%LOCALAPPDATA%\keel\cache`；
- WebView2 的 user data 放 `%LOCALAPPDATA%\klive\WebView2`，与 `cacheDir` 分开。

### 3.6 关闭、托盘与退出

已定行为：**关闭面板不退出，缩到托盘；托盘菜单负责真正退出。**

- 点窗口关闭按钮 → `ShowWindow(SW_HIDE)`，镜像、WebView2、采集线程都继续；
- 托盘图标（`Shell_NotifyIcon`）：
  - 双击 / 左键 → 恢复并激活控制面板；
  - 右键菜单：
    - `显示面板`
    - `停止镜像`（调 `klive_stop_mirror`，页面下一次轮询会看到 `running=false`）
    - `退出`
- `退出` → `klive_stop_mirror()` → `NIM_DELETE` → `DestroyWindow` → UI 线程退出 → `ExitProcess(0)`；
- 宿主主动停用 / 卸载插件时（`onDisable`） → `klive_webview_stop()`：
  停止镜像 → 删除托盘图标 → 销毁窗口 → 退出 UI 线程，但**不** `ExitProcess`；
- `WM_QUERYENDSESSION` / `WM_ENDSESSION` → 先停止镜像，再按退出处理。

关于 `ExitProcess(0)`：

- keel 目前没有「插件请求 Host 停机」的 API；klive 的宿主通常只跑它一个插件，
  所以托盘退出先直接结束进程；
- 这会跳过 keel 的优雅停机，日志尾部可能丢；退出前 `klive_stop_mirror()`、
  `Shell_NotifyIcon(NIM_DELETE)`，并给日志 `Sleep(100)` / `fflush` 一点时间；
- 后续两条改进路线：给 keel 加 `HOST_SHUTDOWN` 通知，或由单 exe 启动器接管退出。

托盘图标资源：`native/webview/klive.ico`，在 `klive_webview.rc` 里用
`IDI_KLIVE ICON "klive.ico"` 嵌入，`LoadIcon(hInstance, MAKEINTRESOURCE(IDI_KLIVE))`。

### 3.7 控制台窗口

暂不隐藏：开发期保留 `keel-host` 控制台，方便看 keel / 插件日志和 `plugins`、`stop` 命令。
交付前如果嫌黑框难看，再由单 exe 启动器（GUI subsystem）或插件里的
`ShowWindow(GetConsoleWindow(), SW_HIDE)` 处理；本方案不把隐藏控制台作为前置条件。

---

## 4. 分阶段实施

### Phase 0：前置与闸门

任务：

1. 已确认 m2 内部文件是新版本（2026-11-12 11:28–11:29），**不需要重新发布**；
2. 在 klive 的 `settings.gradle.kts` 加 `mavenLocal()`（pluginManagement + dependencyResolutionManagement）；
3. 如果 Gradle 仍解析到旧快照，清 `moe.lolosia.keel` 缓存或加 `--refresh-dependencies`；
4. 建一个最小 `keel-plugin-klive` 模块，引用 `PluginContext.materialize` 编译通过；
5. 用 `keelRun` 确认能解析到新的 `keel-host-dev-mingwX64`。

验收：`:keel-plugin-klive:compileKotlinMingwX64` 成功，且能看到新 API 符号。

回滚：不涉及现有代码。

### Phase 1：WebView2 壳与桥（不碰 Kotlin）

任务：

1. `native/webview/klive_webview.h/.cpp`：窗口、WebView2、虚拟主机、MIME 修正、消息循环；
2. 动态加载 `klive_capture.dll`，实现 `enumerateMonitors` / `enumerateWindows` /
   `startMirror` / `stopMirror` / `getStats` / `lastMessage`；
3. `klive_webview_probe` 控制台自检；
4. CMake 加 `klive_webview` target，输出 `Release/klive_webview.dll`；
5. 用 `native/webview/smoke` 或一个最小 HTML 页面验证桥协议。

验收：

- 双击/命令行启动 smoke，窗口打开，页面 `postMessage` 收到回包；
- 枚举显示器/窗口的 JSON 与 `:app:probe` 的输出一致；
- `startMirror` 能在另一台显示器上出现镜像窗口；
- 关闭按钮后窗口隐藏到托盘，镜像继续；托盘退出后镜像停止、进程退出；
- 托盘双击能恢复窗口，右键菜单三项都能工作；
- 在 125% / 150% DPI 下窗口与画面位置正确；
- 虚拟主机能正确返回 `application/wasm`。

回滚：删除 `klive_webview` target 即可；采集核心不动。

### Phase 2：wasm UI 与 Mock

任务：

1. 新建 `ui` 模块（`jvm()` + `wasmJs { browser() }`）；
2. `App.kt` / `Theme.kt` 搬到 `ui/commonMain`；
3. `MirrorEngine` 接口提到 `core`，`app` 的 JNA 实现适配；
4. `ui/wasmJsMain` 加 `Main.kt` + `MockMirrorEngine` + `ComposeViewport`；
5. `bridge.js`、`index.html` 样式、DTO；
6. `:ui:wasmJsBrowserDevelopmentRun` 在 Chrome 里跑通。

验收：

- Chrome 里 UI 与现在 JVM 版布局一致；
- Mock 数据下能切换显示器/窗口、开始/停止、统计轮询；
- `:app:run` 仍然可用（JVM 过渡路径）。

回滚：`app` 不动，删 `ui` 即可。

### Phase 3：插件、物化、打包与端到端

任务：

1. `keel-plugin-klive`：`plugin.json`、`@KeelPluginMain`、`onEnable` 物化 + 启动壳；
2. `:native` 的 CMake 任务迁到 `native/build.gradle.kts`（或临时放插件模块）；
3. 插件模块加 `copyNative` / `copyWeb`，输出到 `build/generated/pluginResources/{native,web}`；
4. `keelPlugin { target("mingwX64"); resources("build/generated/pluginResources") }`；
5. `keelPackage` 依赖 `copyNative` / `copyWeb`；
6. 用 `keelRun`（或 `runHostExecutable`）跑起来；
7. wasm 引擎接真实桥，替换 Mock。

验收：

- `klive-0.1.0.keel` 内容正确；
- `keelRun` 启动后 WebView2 窗口打开，能镜像源屏到目标屏；
- `plugins disable klive` / `enable klive` 后使用新的 `loadSeq` 目录，无文件占用错误；
- 关闭按钮隐藏到托盘；托盘 `退出` 停止镜像并结束进程；
- 宿主重启后旧 `loadSeq` 目录被清理。

回滚：删 `keel-plugin-klive`，保留 `app`。

### Phase 4：清理与文档

任务：

1. 删除 `app`、JNA、`compose.desktop`、MSI 打包；
2. `core` / `ui` 去掉 `jvm()`；
3. `--probe` 移到插件命令（`klive probe`）；
4. 更新 `AGENTS.md`（目录、构建、验证）、`README.md`、`doc/项目记忆.md`；
5. 记录实测：`.keel` 体积、启动时间、内存、镜像帧率。

验收：仓库里不再有 JVM 运行时依赖；文档命令与实际一致。

---

## 5. Gradle / CMake 改动（骨架）

### 5.1 `settings.gradle.kts`

```kotlin
pluginManagement {
    repositories {
        mavenLocal()
        // 现有阿里云 / gradlePluginPortal ...
    }
}

dependencyResolutionManagement {
    repositories {
        mavenLocal()
        // 现有仓库 ...
    }
}

include(":core")
include(":ui")
include(":native")
include(":keel-plugin-klive")
```

`app` 模块已在 Phase 4 删除，`settings.gradle.kts` 不再 include 它。

### 5.2 `gradle/libs.versions.toml`

```toml
[versions]
keel = "0.1.0-SNAPSHOT"
kotlinx-serialization = "1.9.0"   # 与 Kotlin 2.4 兼容的版本，实施时确认
# kotlinx-coroutines 已有，沿用 1.11.0

[libraries]
kotlinx-serialization-json = { module = "org.jetbrains.kotlinx:kotlinx-serialization-json", version.ref = "kotlinx-serialization" }
kotlinx-coroutines-core = { module = "org.jetbrains.kotlinx:kotlinx-coroutines-core", version.ref = "kotlinx-coroutines" }

[plugins]
kotlinSerialization = { id = "org.jetbrains.kotlin.plugin.serialization", version.ref = "kotlin" }
```

### 5.3 `core/build.gradle.kts`

```kotlin
kotlin {
    jvm()
    wasmJs { browser() }
    mingwX64()
    linuxX64()
}
```

### 5.4 `ui/build.gradle.kts`

```kotlin
plugins {
    alias(libs.plugins.kotlinMultiplatform)
    alias(libs.plugins.composeMultiplatform)
    alias(libs.plugins.composeCompiler)
    alias(libs.plugins.kotlinSerialization)
}

kotlin {
    jvm()
    wasmJs { browser() }

    sourceSets {
        commonMain.dependencies {
            api(project(":core"))
            implementation(compose.runtime)
            implementation(compose.foundation)
            implementation(libs.compose.material3)
            implementation(libs.kotlinx.coroutines.core)
        }
        jvmMain.dependencies {
            implementation(compose.desktop.currentOs)
        }
        wasmJsMain.dependencies {
            implementation(libs.kotlinx.serialization.json)
        }
    }
}
```

### 5.5 `keel-plugin-klive/build.gradle.kts`

```kotlin
plugins {
    alias(libs.plugins.kotlinMultiplatform)
    id("moe.lolosia.keel.plugin")
}

group = "moe.lolosia.klive"
version = "0.1.0"

keelPlugin {
    target("mingwX64")
    buildType = "DEBUG"
    resources("build/generated/pluginResources")
}

kotlin {
    mingwX64 { binaries.sharedLib { baseName = "keel-plugin-klive" } }

    sourceSets {
        all { languageSettings.optIn("kotlinx.cinterop.ExperimentalForeignApi") }
        commonMain.dependencies {
            api("moe.lolosia.keel:keel-api:0.1.0-SNAPSHOT")
        }
    }
}
```

`plugin.json`：

```json
{
  "id": "klive",
  "name": "klive",
  "version": "0.1.0",
  "abiVersion": 1,
  "entry": "keel_plugin_worker_main",
  "lib": { "mingwX64": "lib/mingwX64/keel_plugin_klive.dll" },
  "resources": "resources/",
  "requires": [],
  "authors": ["lolosia"]
}
```

### 5.6 `native/CMakeLists.txt`

```cmake
add_library(klive_webview SHARED
    webview/klive_webview.cpp
)
target_include_directories(klive_webview PRIVATE
    webview
    webview/third_party
)
target_compile_definitions(klive_webview PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
)
target_link_libraries(klive_webview PRIVATE
    ole32 oleaut32 user32 gdi32 shell32
)
```

默认按官方 loader 接入：`native/webview/third_party/webview2/` 放 `WebView2.h`、
`WebView2EnvironmentOptions.h`、`WebView2Loader.lib`；构建后把 `WebView2Loader.dll`
一并复制进 `.keel` 的 `resources/native/`。如果以后改用 `OpenWebView2Loader`，
再把 `Source/WebView2Loader.cpp` 加进 target、加对应 Include 路径，并去掉 loader DLL。

### 5.7 资源复制

```kotlin
val copyNative by tasks.registering(Sync::class) {
    dependsOn(":native:nativeBuild")
    from(rootProject.layout.projectDirectory.dir("native/build/cmake/Release")) {
        include("klive_capture.dll", "klive_webview.dll", "WebView2Loader.dll")
    }
    into(layout.buildDirectory.dir("generated/pluginResources/native"))
}

val copyWeb by tasks.registering(Sync::class) {
    dependsOn(":ui:wasmJsBrowserDistribution")
    from(rootProject.layout.projectDirectory.dir("ui/build/dist/wasmJs/productionExecutable"))
    into(layout.buildDirectory.dir("generated/pluginResources/web"))
}

tasks.named("keelPackage") {
    dependsOn(copyNative, copyWeb)
}
```

实施时用 `.\gradlew.bat :ui:tasks --all` 确认 wasm 产物任务名与输出目录。

---

## 6. 验证矩阵

| 场景 | 命令 / 操作 | 期望 |
| --- | --- | --- |
| 旧链路不回归 | `.\gradlew.bat :app:probe` | 仍能枚举 3 台显示器与窗口 |
| UI 开发 | `.\gradlew.bat :ui:wasmJsBrowserDevelopmentRun` | Chrome 里 UI 正常，Mock 数据可交互 |
| 原生壳 | `klive_webview_probe` / smoke | 枚举 JSON 正确，镜像可启动 |
| 打包 | `.\gradlew.bat :keel-plugin-klive:keelPackage` | `.keel` 内含 plugin.json / lib / resources/native / resources/web |
| 端到端 | `.\gradlew.bat :keel-plugin-klive:keelRun` | Host 启动、WebView2 窗口打开、镜像可开始/停止 |
| 重载 | 控制台 `plugins disable klive` / `enable klive` | 新 `loadSeq` 目录，无文件占用错误 |
| 清理 | 重启 Host | 旧 `loadSeq` 目录被删除 |
| 关闭按钮 | 点窗口 X | 窗口隐藏，镜像继续，托盘图标存在 |
| 托盘恢复 | 双击托盘 | 窗口显示并激活 |
| 托盘停止 | 托盘菜单「停止镜像」 | 镜像停止，面板统计变为空闲 |
| 托盘退出 | 托盘菜单「退出」 | 镜像停止、托盘移除、进程退出 |
| DPI | 125% / 150% | 窗口、DPI、目标屏定位正确 |
| 缺 Runtime | 无 WebView2 的机器 | 明确提示，不崩溃 |
| 体积 | 查看 `.keel` 与解包后目录 | 记录 wasm + DLL 体积，作为后续优化基线 |

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| Gradle 缓存旧快照 | 编译不到 `cacheDir` / `materialize` | m2 已确认新版本；必要时清缓存 / `--refresh-dependencies` |
| Compose wasm 在 WebView2 里白屏 | UI 不可用 | Phase 2 先在 Chrome 跑通；WebView2 里开 DevTools 看 console |
| `.wasm` MIME 不对 | `instantiateStreaming` 失败 | `WebResourceRequested` 显式设置 `application/wasm` |
| WebView2 Runtime 缺失 | 无法打开 UI | 启动前检测 + MessageBox + 文档说明 |
| 桥调用异步 | UI 状态错乱 | 请求 id + 5s 超时；Mock 引擎覆盖 UI 开发 |
| 重载 DLL 被占用 | 插件启用失败 | 依赖 keel 的 per-load 目录；不要自己改名依赖 |
| MSVC / MinGW 混用 | 链接失败 / 运行崩溃 | 只走 C ABI DLL；不静态链接 |
| CRT 混用 | 跨 DLL 释放内存崩溃 | 每个 DLL `/MT` 或随包带 VC 运行时；跨边界只传 POD / 调用方缓冲 |
| 64 位句柄精度 | 窗口句柄错乱 | JSON 用字符串 |
| 控制台黑框 | 用户观感差 | 暂不隐藏，先保证调试；交付前再处理 |
| 托盘退出用 `ExitProcess` | 跳过 keel 优雅停机、日志尾部可能丢 | 退出前 stop + `NIM_DELETE` + `Sleep(100)`；后续加 `HOST_SHUTDOWN` 或单 exe |
| 托盘图标/资源缺失 | 托盘不显示 | `.rc` 嵌入 `klive.ico`；启动时校验 `LoadIcon` 返回值 |
| `.keel` 体积 | 首次物化慢 | 记录基线；必要时裁剪 wasm / 延迟加载 |

---

## 8. 已定与待确认

已定：

1. **WebView2 loader**：官方 `WebView2Loader.dll`，随 `.keel` 物化；
   `OpenWebView2Loader` 留作以后减少一个 DLL 的优化项。
2. **关闭行为**：关闭按钮隐藏到托盘；托盘菜单负责显示 / 停止 / 退出。
3. **控制台**：暂不隐藏，保留 keel 日志与控制台命令。
4. **构建类型**：先 `DEBUG`（`enable_devtools = true`），验收后再切 `RELEASE`。
5. **过渡期**：保留 `app` 与 JVM 路径，直到 `.keel` 端到端验收通过。

待确认：

6. **插件 id / 版本**：`klive` / `0.1.0` 是否就是最终值？
7. **WebView2 Runtime 分发**：只要求预装，还是随包带 Evergreen Bootstrapper？
8. **托盘退出方式**：是否要让 keel 增加 `HOST_SHUTDOWN` 通知，替换 `ExitProcess(0)`？

---

## 9. 完成定义

- `.keel` 在干净的 keel Host 上启动，打开 WebView2 控制面板；
- 能镜像显示器 / 窗口到目标屏，统计、缩放、垂直同步与现在一致；
- 运行时不再需要 JVM、JNA、Compose Desktop；
- 关闭按钮缩到托盘，托盘可恢复 / 停止镜像 / 退出；
- 重载、托盘、DPI、缺 WebView2 Runtime 都有可读行为；
- `AGENTS.md` / `README.md` / `doc/项目记忆.md` 与实际命令一致。
