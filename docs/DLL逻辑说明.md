# PlusPlugin DLL 当前逻辑说明

> 本文是**现状描述**（代码读出来的事实），不是待办计划。开发/里程碑/踩坑见 `改造开发-进度与交接.md`。
> 对应源码：`../proxy/`（工作区）= `C:\native\proxy\`（VM 编译机）。

---

## 1. 一句话概括

把 `version.dll` 放进 `%APPDATA%\TTVoice\`，利用 Windows 的 DLL 搜索顺序让 `TTVoice.exe` 加载**我们的代理**而不是系统的那一个；代理把 17 个导出原样转发给 `System32\version.dll`，同时在自己的进程空间里：

1. 给 D3D11 设备补 `BGRA_SUPPORT`；
2. 钩住 `IDXGISwapChain::Present`，在应用自己的后备缓冲上画一个胶囊按钮 + 左下角日志；
3. 子类化 `FLUTTERVIEW` WndProc 收鼠标，点击后**只跑 JSON 脚本**（ini 里硬编码的两段点击链已整体删除，没脚本时点下去没有动作）；
4. 所有动作都是往 `FLUTTERVIEW` `PostMessage` 合成鼠标消息——不移动真实鼠标、不要求前台、不改应用任何文件。

---

## 2. 工程文件与产物

| 文件 | 行数 | 职责 |
|---|---|---|
| [`dllmain.cpp`](file:///D:/AIProjects/PlusPlugin/native/proxy/dllmain.cpp) | 196 | 代理本体：解析真身导出、`Log()`、找主窗口、起线程 |
| [`overlay.inc`](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc) | 1567 | Present hook、绘制、输入子类化、视觉门控抓帧/判色、ini 配置、D3D11 BGRA hook |
| [`script_engine.inc`](file:///D:/AIProjects/PlusPlugin/native/proxy/script_engine.inc) | 833 | JSON 精简解析、脚本选择/热重载、逐行执行、全局热键 |
| [`stubs.asm`](file:///D:/AIProjects/PlusPlugin/native/proxy/stubs.asm) | 93 | 17 个 `jmp qword ptr [g_pfn_X]`（**auto-generated**） |
| [`exports.def`](file:///D:/AIProjects/PlusPlugin/native/proxy/exports.def) | 21 | `LIBRARY version` + 17 个导出名（**auto-generated**） |
| [`proxy_exports.h`](file:///D:/AIProjects/PlusPlugin/native/proxy/proxy_exports.h) | 23 | X-macro 导出列表，驱动「定义指针」和「解析指针」两处（**auto-generated**） |
| [`build.bat`](file:///D:/AIProjects/PlusPlugin/native/proxy/build.bat) | 47 | `vswhere` → `vcvars64` → `ml64` → `cl /LD /MT /O2 /EHsc /W4 /utf-8` |

`.inc` 是被 `#include` 进 `dllmain.cpp` 的（`overlay.inc` 在 `namespace ov` 内，`overlay.inc` 自己又在 [L912](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L912) include `script_engine.inc`），所以整个 DLL 只有**一个编译单元**，所有符号都是 `static`。三个源文件必须**纯 ASCII**（中文一律 `\xXXXX` 转义），这是踩坑 1 的硬约束。

`native/dwmapi_proxy/` 是早期同类代理，载荷只到「找到窗口 + 打印 `payload ready (hook placeholder)`」就结束了（[dllmain.cpp L167-L170](file:///D:/AIProjects/PlusPlugin/native/dwmapi_proxy/dllmain.cpp#L167-L170)），**没有任何叠加绘制或点击逻辑，也不参与部署**——它留着只作为「为什么最后选了 version.dll」的历史证据（踩坑 12：dwmapi 代理会在 DllMain 里提前拉起 dcomp/图形栈，导致「序数 137 于 d3d9.dll」弹窗）。

---

## 3. 启动时序

```
TTVoice.exe 启动
  └─ 加载 version.dll（我们的）
       ├─ DllMain(DLL_PROCESS_ATTACH)                      [dllmain.cpp L180]
       │    ├─ DisableThreadLibraryCalls
       │    ├─ ResolveRealDll()  ← 同步：LoadLibraryW(System32\version.dll)
       │    │     并 GetProcAddress 填 17 个 g_pfn_*，缺失数存 g_resolveMiss
       │    ├─ 若 exe 名 == TTVoice.exe → ov::StartD3D11Hook()   （尽早，先于 ANGLE 建设备）
       │    └─ CreateThread(PayloadThread)
       │
       └─ PayloadThread                                     [dllmain.cpp L137]
            ├─ Log("attached exe=%ls resolve_miss=%d")
            ├─ exe 名不是 TTVoice.exe → Log("skip") 直接退出（代理仍正常转发）
            ├─ 等 dxgi.dll：最多 3000 × 10ms；超时 → "WARN: dxgi.dll not found in 30s" 退出
            ├─ 等主窗口：EnumWindows 找自己 PID + 可见 + class
            │  「FLUTTER_RUNNER_WIN32_WINDOW」，最多 600 × 50ms
            ├─ Log 窗口 rect 与 GetDpiForWindow
            └─ ov::Init(hwnd)
```

`ov::Init`（[overlay.inc L1543](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L1543)）五步，顺序有意义：

| 步骤 | 做什么 | 失败后果 |
|---|---|---|
| `g_scale = dpi/96` | 用 `GetDpiForWindow(g_hwnd)`，取不到则按 96 | scale 错 → 所有坐标/字号整体偏 |
| `LoadConfig()` | 读 `%APPDATA%\TTVoice\overlay.ini`；**不存在就写模板**；老 ini 缺 `log_*` 键会在文件尾追加（追加/残留键的 `strstr` 探测必须在解析循环**之前**做完 —— 循环会把每个 `=` 原地改成 NUL，事后再查 `"log_on_screen="` 永远失败，曾经每次启动都追加一段） | 全部用编译期默认值 |
| `ResolveFontPath()` | `%ENV%` 展开 + 相对路径按 exe 目录拼绝对 | 后面字体回退到系统集合 |
| `InstallHooks()` | 造一次性 swapchain 拿 vtable 并打补丁 | **返回 false → `g_ready` 保持 false，叠加层和脚本引擎全都不启动** |
| `ScriptStartHotkey()` | 起脚本轮询/热键线程 | — |

`InstallHooks` 之后才 `g_ready = true`，所以「Present 钩子装好」是绘制和脚本的前提。

---

## 4. 图形接管：两层钩子

### 4.1 D3D11 设备补 BGRA（`StartD3D11Hook`，独立线程）

ANGLE 建 device 时不带 `D3D11_CREATE_DEVICE_BGRA_SUPPORT`，导致 `ID2D1Factory1::CreateDevice` 报 `E_INVALIDARG`。修法是给 `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` 的 `flags` 按位或上这个标志，两条拦截路径并用：

- **IAT 补丁**：`PatchIatForModule` 遍历模块导入表，只认**按名字**的导入；把 `d3d11.dll!D3D11CreateDevice(AndSwapChain)` 和任意模块的 `GetProcAddress` 换成我们的包装。触发点有两处：启动时 `PatchAllD3D11Iat()` 扫一遍已加载模块 + `LdrRegisterDllNotification` 在新模块加载瞬间补（回调在 loader lock 下，刻意不分配内存不打日志）。另外还有一个 100 × 100ms 的兜底重扫，覆盖延迟解析导入的模块。
- **`GetProcAddress` 拦截**：`libGLESv2` 是动态加载 `d3d11.dll` 的，IAT 里没有那些符号，所以必须钩 `GetProcAddress`，当 `mod == g_d3d11Module` 且是名字导入时返回包装函数（拦截处 `HookGetProcAddress` [L1204](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L1204)，替换 IAT 槽在 [L1272](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L1272)）。

首次命中打一行 `d3d11: CreateDevice flags=0x%X -> add BGRA` 或 `gpa: D3D11CreateDevice intercepted`，都只打一次。

### 4.2 Present vtable hook

`InstallHooks` 注册一个 4×4 的 `TTProxyDummyWnd`，用 `D3D11CreateDeviceAndSwapChain`（HARDWARE 失败退 WARP）拿到一个 swapchain，**只为了取得共享的 vtable**，然后 `PatchVTable` 改三个槽位（`VirtualProtect` 前后各一次）：

| 槽位 | 原函数保存于 | 替换为 | 作用 |
|---|---|---|---|
| `vt[8]` | `g_origPresent` | `HookPresent` | 每帧触发绘制 |
| `vt[13]` | `g_origResize` | `HookResize` | `ResizeBuffers` 时 `ResetTargets()` |
| `vt[22]` | `g_origPresent1` | `HookPresent1` | 同 `HookPresent` |

因为是**按 vtable 改**，进程内所有 swapchain 都会进来，所以每帧都要认人：

```
HookPresent(sc) → OnPresent(sc)                    [overlay.inc L1035]
  ├─ !g_ready → 返回
  ├─ IsTargetSwapChain(sc, &view)                  [L997]
  │    GetDesc() 拿 OutputWindow
  │    ├─ IsRelatedWindow(OutputWindow)？          [L768]
  │    │     == g_hwnd | GetAncestor(GA_ROOT)==g_hwnd | GetWindow(GW_OWNER)==g_hwnd
  │    │     命中 → g_hwndMatched = true，输出该 hwnd
  │    ├─ 否则首次打一行 "first swapchain: out=... cls=... w= h= fmt=..."
  │    └─ OutputWindow 为空且从没匹配过主窗口时：退化成「缓冲区尺寸 == 主窗口客户区尺寸」
  ├─ 首次命中打 "present hook hit"（g_hits 只记一次）
  ├─ EnsureViewSubclass(view)   ← 见到 FLUTTERVIEW 就子类化，一次
  └─ Draw(sc)
```

实测该应用的 swapchain 挂在 `FLUTTERVIEW` 子窗口上而不是主窗口（踩坑 10），所以 `IsRelatedWindow` 三种关系都必须认。

---

## 5. 绘制

`Draw`（[L759](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L759)）：`EnsureDevice` → 首次 `InitD2D` → `DrawD2D`，返回 false 才 `DrawFallback`。

**flip-model 缓存**：ANGLE 是多缓冲轮转，`GetBuffer(0)` 每次可能给不同纹理，所以 `ObtainBuffer` 按「纹理指针 + 宽高」缓存最多 4 个 `BufEntry{tex, rtv, bmp, w, h}`；超了就复用第 0 项重建。`ResizeBuffers` 时整表清空。

### 5.1 D2D 主路径（`DrawD2D`，[L664](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L664)）

变换矩阵强制 Identity（自己乘 `g_scale`），画两坨东西：

**胶囊按钮**，矩形来自 `GetButtonRectPx`：`x/y/w/h`（逻辑 px）× `g_scale`，逐层叠加

1. 底色：仅 `bg_alpha > 0` 才 `FillRoundedRectangle`（默认 `bg_alpha=0` = 静止态完全透明，露出应用顶栏）；
2. 高亮：press 叠 `白 0.14`、hover 叠 `白 0.07`；
3. 描边：`border_width > 0` 才画，press 时 RGB 各乘 0.82；
4. 文字：`g_noScriptFile ? "无脚本" : text`（见 §8.3），居中、不换行。
   圆角 `(g_radius > 0 ? g_radius : h/2) × g_scale`。

**左下角日志**（`DrawScreenLog`，见 §9）。

`EndDraw` 失败 → `ResetTargets()` 并返回 false，下一帧自动走兜底路径。

### 5.2 D3D11 兜底（`DrawFallback`，[L727](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L727)）

只用 `ClearView` 往 RTV 刷两块纯色矩形：按钮主体（press 时亮 0.08）+ 顶部 6 物理 px 的强调条。**没有任何文字，也没有界面日志**。第一次进入打一行 `overlay: using d3d11 fallback (no text)`，看到它就意味着「界面上看不到状态提示」是正常的。

### 5.3 字体链（`InitTextFormat`，[L327](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L327)）

优先级从高到低，任一级成功即止：

1. `InitFontFromFile`：DirectWrite 3 从 `data\flutter_assets\web\fonts\NotoSansSC.ttf`（应用自带）建**私有字体集**（`IDWriteFactory5 → CreateFontSetBuilder → CreateFontCollectionFromFontSet`），不装字体、不碰系统集合；配置的 `font_family` 和文件里的名字不符时，枚举该集合的真实家族名重试并回写 `g_fontFamily`；
2. 系统集合 + `font_family`；
3. 系统集合 + `Microsoft YaHei`；
4. 全失败 → `font: no text format available`（此时 D2D 仍会画形状，只是没字）。

`font_size` 是**逻辑 px**，建 `IDWriteTextFormat` 时乘 `g_scale`（踩坑 13 的修法）。日志格式 `g_logFmt` 与按钮 `g_textFmt` 分开建（同一字集、更小字号、左/顶对齐、不换行），互不干扰。

---

## 6. 输入接管

`EnsureViewSubclass`（[L979](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L979)）在 Present 回调里对 `FLUTTERVIEW` 做一次 `SetWindowLongPtrW(GWLP_WNDPROC)`，原过程存 `g_origViewProc`；`SetWindowLongPtr` 失败只打一次 `overlay: SetWindowLongPtr failed, no click handling`。之后 `ViewProc` 在 UI 线程被调用（P2-1 已实测点击可用）。

`ViewProc`（[L928](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L928)）的分流原则是**只吃按钮矩形内的事件**，其余一律转发：

| 消息 | 矩形内 | 矩形外 |
|---|---|---|
| `WM_MOUSEMOVE` | 首次进入时 `TrackMouseEvent(TME_LEAVE)`；更新 `g_hover` | 更新 `g_hover=0` |
| `WM_LBUTTONDOWN` | `g_pressed=1`，**返回 0（吞掉）** | 转发给 Flutter |
| `WM_LBUTTONUP` | 若之前 pressed：落在内 → `g_clicks++` + `overlay: button clicked` + `OnButtonClick()`；无论如何**返回 0** | 转发 |
| `WM_MOUSELEAVE` | `g_hover = g_pressed = 0` | — |

命中判定 `HitTestButton` 用的就是 §5.1 那个 `×g_scale` 后的矩形，和画出来的位置严格一致。

---

## 7. 点击之后跑什么（核心分流）

`OnButtonClick()`（[L922](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L922)）就是全部逻辑：

```cpp
(void)ScriptTryRun(true);   // verbose：抢不到东西时也要把原因说出来
```

ini 里硬编码的两段点击链（`click_x` / `click_y` / `click2_*` / `click_delay_ms` 及
`ClickChainThread` / `SimulateClickAt` / `g_chainActive` / `kMinBannerPixels`）已整体删除，
所以按钮**只有一个动作**：跑脚本。

`ScriptTryRun(verbose) = ScriptReload(verbose) && ScriptRun()`，三条**容易踩错**的语义：

- `ScriptReload()` 返回 false（没脚本 / 脚本被拒收）→ **点下去真的什么都不发生**，只剩常驻状态行（§8.3）与胶囊文字；
- 这时日志里**每次点击都会重打一行原因**（`verbose=true` 绕过 `g_loggedNoScript` 的一次性抑制；被拒收的文件即使没修改也会重复打 `script: parse failed ...`，原因存在 `g_scriptErr`）—— 2s 轮询线程传 `false`，那里仍是一次性，否则日志每两秒刷一行；
- `ScriptRun()` 在已有 run 活跃时返回 **true**（并打 `script: run ignored, previous run still active`）→ 连点被挡住，但也不会「退化成别的动作」。

### 7.1 合成点击的消息序列

`SimulateClickKindAt`（脚本用的那个）坐标一律 `static_cast<LONG>(逻辑 × g_scale)`（**截断**，不是四舍五入）：

| 动作 | 消息序列 |
|---|---|
| 左键 | `WM_MOUSEMOVE → WM_LBUTTONDOWN(MK_LBUTTON) → WM_LBUTTONUP(0)` |
| 双击 | 上面 down/up 之后 `Sleep(40)`，再 `WM_LBUTTONDBLCLK(MK_LBUTTON) → WM_LBUTTONUP` |
| 右键 | `WM_MOUSEMOVE → WM_RBUTTONDOWN(MK_RBUTTON) → WM_RBUTTONUP` |
| 滚轮 | 坐标先 `ClientToScreen`（`WM_MOUSEWHEEL` 吃**屏幕**坐标），`wParam = (120 × 格数) << 16` |
| 粘贴 | `OpenClipboard → EmptyClipboard → SetClipboardData(CF_UNICODETEXT) → CloseClipboard` → `SetForegroundWindow` → `Sleep(80)` → `SendInput` 四步 Ctrl+V |

目标窗口优先 `g_viewHwnd`（`FLUTTERVIEW`），没有才退回主窗口。全部 `PostMessage`，不移动真实鼠标。

### 7.2 视觉门控（为什么不用固定延时）

「要等上一页跳完才出现的元素」不能靠固定延时点：实测 600ms 稳定偏早（踩坑 15）。门控只有 `WaitForCreamAt` 一处（[script_engine.inc L680](file:///D:/AIProjects/PlusPlugin/native/proxy/script_engine.inc#L680)），由每行的 `wait_px` 调用；它复用 `overlay.inc` 里的 `CaptureView` / `CountCreamBand`：

```
limit = wait_max_ms > 0 ? wait_max_ms : 4000
poll  = gate_poll_ms < 30 ? 120 : 值        （ini 键，默认 120）
每轮：while (未超 limit) → Sleep(poll) 先睡 → 抓帧判色
命中：奶油色像素 >= wait_px 且连续 3 次命中（≈240ms 不动）
      → "script: gate hit Npx at X,Y after Tms"，放行本行
超时 → "script: gate timeout Nms (max Kpx)"，返回 -1，但调用方不看返回值 → 本行照点
```

抓帧与判色：`CaptureView` 用 `PrintWindow(view, dc, PW_RENDERFULLCONTENT)` 打到一个**顶朝下 32bpp DIB**（尺寸没变就复用），`CountCreamBand` 在目标点周围 **±200 × ±40 物理 px** 的带里数 `R>228 && G>210 && B<228 && R-B>22` 的像素，并返回命中质心（日志里那个 `at 2359,131` 就是它，用来区分「没出现」和「位置变了」）。连续 3 次是为了滤掉淡入/滑入的中间帧。

旧的 `ClickChainThread` 是「先判断再睡」，`WaitForCreamAt` 是「先睡再判断」——编辑器 `../script_editor.py` 复刻的是后者（含日志字样），见 §10 的 parity 表。

---

## 8. 脚本引擎（`script_engine.inc`）

### 8.1 选脚本 + 热重载

`ScriptResolvePath`（[L445](file:///D:/AIProjects/PlusPlugin/native/proxy/script_engine.inc#L445)）三档优先级：

1. `overlay.ini` 的 `script=<名>`（不带 `.json` 会自动补）→ 文件不存在**直接判「没脚本」，不会继续往下看后两档**；
2. `scripts\active.txt`：只读**前 159 字节**的首行，无条件补 `.json`；文件不存在则静默降到第 3 档；
3. `scripts\*.json` 里**修改时间最新**的那个（`>=` 比较，同时间取后遍历到的）。

`ScriptReload(bool verbose)`（[L512](file:///D:/AIProjects/PlusPlugin/native/proxy/script_engine.inc#L512)）的三条短路：

- `g_scriptActive != 0`（正在跑）→ 直接返回 `g_script.count > 0`，**绝不在运行中途换脚本**；
- 路径没变且 `ftLastWriteTime` 没变 → 复用已解析结果（一次 `GetFileAttributesExW`，很便宜）；但 `verbose` 且上次是被拒收 → 重复打一行 `script: parse failed ...`（§7）；
- 否则重读重解析，成功就 `g_script = parsed`（`g_scriptLock` 独占）并清空 `g_scriptErr`，失败把 `count` 清零并把原因存进 `g_scriptErr[64]`。

运行侧：`ScriptRunThread` 一开始在**共享锁**下把 `g_script` 拷成局部 `local`，之后全程用副本 —— 热重载不可能把一次正在进行的执行撕坏。

### 8.2 JSON 契约（精简解析器）

Schema：`{version, name, hotkey, loop, events:[{x,y,click,delay_ms,wheel,text,wait_px,wait_max_ms}, ...]}`

| 规则 | 实现 |
|---|---|
| 事件上限 | `kScriptMaxEvents = 32`，**超出部分 `JsonSkip` 静默丢弃**（不报错） |
| 文本上限 | `kScriptText = 256` 个宽字符，`Utf8ToWideN` 截断而非失败 |
| `loop` | `<1 → 1`，`>99 → 99` |
| `name` | 只留 63 个宽字符 |
| `click` | 字符串：`"double"→1`、`"right"→2`、其他/缺省 → 0（左键） |
| `hotkey` | 字符串：`F1..F24`、单字母、单数字；其余 = 0（不注册） |
| 未知键 | `JsonSkip`（所以编辑器的 `view_size` DLL 根本不认） |
| 字符串解码 | 支持 `\n \t \r \b \f \uXXXX` 与代理对（合成非 BMP），输出 UTF-8 再转 UTF-16 |
| **成败判据** | `return out->count > 0;` —— `events: []` 视为失败，报 `no events / bad json` |
| 文件判据 | `size <= 0 \|\| size > 1MB` → `bad size`；打不开 → `open failed`；开头 UTF-8 BOM 会跳过 |

### 8.3 每行执行顺序与状态显示

```
for round in 1..loop:
  for i in 0..count-1:
     wait_px > 0        → WaitForCreamAt(x, y, wait_px, wait_max_ms, poll)   # 超时也照样往下走
     wheel != 0         → SimulateWheelAt
     text 非空          → PasteTextToApp
     总是               → SimulateClickKindAt(x, y, click)
     本轮最后一行之外   → Sleep(delay_ms)      # 整份脚本的最后一行不 delay
```

同时 `ScriptReload` 的三个出口会写「脚本状态」，由 `SetScriptStatus`（[overlay.inc L434](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L434)）落到界面上：

| code | 触发条件 | 常驻状态行 | 胶囊文字 |
|---|---|---|---|
| 1 | 一个脚本文件都没找到（含 `script=` 指空） | 「没有激活的脚本，点按钮不会有动作」 | 换成 `no_script_text`（默认「无脚本」） |
| 2 | 文件在但被拒收（坏 JSON / 事件表空 / 坏尺寸） | 「脚本不可用（&lt;原因&gt;），点按钮不会有动作」 | **不变** |
| 0 | 脚本正常加载 | 清空 | 不变 |

设计原则：**只有「根本没有脚本文件」才动胶囊**，其他异常一律只在日志里说，不顶掉用户调好的顶栏外观。

### 8.4 热键与 2 秒轮询线程

`ScriptHotkeyThread`（[L786](file:///D:/AIProjects/PlusPlugin/native/proxy/script_engine.inc#L786)）是一个死循环：

```
每轮：ScriptReload(false)                     ← 热重载的实际驱动，不只在按钮点击时；静默防屏
      vk 变了且没试过 → Unregister 旧的、RegisterHotKey(nullptr, 0x5454, 0, vk)
                        成功打 "script: hotkey vk=0x%02X registered"
                        失败打 "... busy, not registered"
      MsgWaitForMultipleObjects(0, ..., 2000, QS_ALLINPUT)
      排空消息，WM_HOTKEY && wParam==0x5454 → "script: hotkey pressed" → ScriptReload(true) + ScriptRun()
                                        ↑ 热键是主动动作，跟按钮一样 verbose
```

也就是说：**没有任何脚本文件时，DLL 靠这个线程每 2 秒刷新一次胶囊文字与状态行**（编辑器里保存新脚本后最多 2 秒生效）；而 `ScriptStartHotkey` 是在 `ov::Init` 里**无条件**启动的，脚本没配 hotkey 时该线程仍然负责轮询。

---

## 9. 日志

两条输出，同一个入口 `ov::LogFromOverlay`（[dllmain.cpp L85](file:///D:/AIProjects/PlusPlugin/native/proxy/dllmain.cpp#L85)）：

```
LogFromOverlay(text) → Log()        写 proxy.log（默认 version.dll 同目录 %APPDATA%\TTVoice\，回退 %TEMP%），前缀 [HH:MM:SS.mmm][pid][tid]
                   → PushScreenLog() 进界面环形缓冲（自己拼 [HH:MM:SS.mmm] 前缀，见 §9）
```

**界面日志**（`log_on_screen=1`，[overlay.inc L346-L551](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L346-L551)）：本质是左下角的一个 **tail -f 面板**。

- 环形缓冲 `24 × 200` 个宽字符，满了 `memmove` 丢最旧的一条（**下标 0 永远是最旧、末尾永远是最新**，所以绘制时取尾部 `log_lines` 行就是天然的时间序）；跨线程写入全程 `SRWLOCK`；
- **每一行都带 `[HH:MM:SS.mmm]` 前缀**（`StampPrefix` [L382](file:///D:/AIProjects/PlusPlugin/native/proxy/overlay.inc#L382)，与日志文件同一个时钟）：这不是装饰 —— 同一个错误反复出现时（连点「活动」每次都是 `overlay: button clicked` + `script: no json found ...`），正文逐字符相同，没有时间戳的话 6 行窗口里的内容永远一模一样，**环形缓冲在滚但眼睛看不出来**，会被当成「面板卡住了」；
- 长行只截不断：`kStampLen = 16` 是给前缀留的位，剩下的 `184` 字容不下的部分就地截断（不用 `strncpy_s(_TRUNCATE)`，它溢出时会走 CRT invalid-parameter 处理器 → 直接带死宿主进程）；
- 每帧在共享锁里取**快照**（常驻状态行 + 最近 `log_lines` 行），绘制时只碰本地数组；
- 位置：左下角，边距 12 逻辑 px，半透明黑底 `#0A0D12 α0.78` 圆角 5 逻辑 px，宽度 `min(画面 55%, 980 逻辑 px)`，小于 120 逻辑 px 直接不画；
- **不淡出（本次改动）**：旧的 `log_seconds` 计时隐藏已整体删除（`g_logSeconds` / `g_screenLogStamp` 都没了）；面板一旦有内容就常驻，新行永远画在底部、把最旧的一条推出窗口 —— 就是 tail -f。旧 ini 里残留的 `log_seconds=` 会被忽略，但 DLL 会打一行 `config: log_seconds is gone, the log panel stays on screen`；
- 面板高度 = 当前行数（状态行 + `min(log_lines, 已有行数)`）：底边钉在左下角，行数变多时往上长，所以不会抖位置；
- 琥珀色：`g_statusLine`（第一行强制）或 `IsWarnLine` 命中 `fail / timeout / busy / ignored / skipped / WARN / not seen / no json` 任一片段（时戳前缀不影响 `wcsstr` 判定）；其余浅灰白。
- 常驻状态行也带时戳，含义是「这个状态最后一次变化的时刻」（`SetScriptStatus` 只在三个出口调用，mtime 未变时不重写，所以它一般不动）。
- 一次点击至少两行（均带时戳，以 150% 屏为例）：

```
[23:41:17.485] overlay: button clicked
[23:41:17.486] script: no json found, the button does nothing     ← 琥珀
```

  跑不起来时是 `no json found` / `parse failed`（§7 的 verbose），成功时是 `script: run ...` 系列。
- 取不到 `g_logFmt`/`g_logBrush` 时自动把 `g_logOnScreen` 关掉并打 `log: no text format for the on-screen log`，按钮不受影响。

`log_lines` 默认 **6**、解析时 clamp 到 `0..24`（缓冲就 24 行，画出来最多 1+24=25 行）；`log_lines=0` **只停掉镜像行**，常驻状态行仍会画（整块关掉是 `log_on_screen=0`）；`log_size` 只有在 `6 < v < 40` 时才采纳；`no_script_text=` 留空 = 保留默认「无脚本」。

---

## 10. 线程与同步

| 线程 | 创建处 | 干什么 | 触碰的共享状态 |
|---|---|---|---|
| DllMain 调用线程 | — | 只解析转发指针 + 起线程 | `g_pfn_*` |
| `D3D11HookThread` | `StartD3D11Hook` | 装 BGRA 钩子、扫 IAT、注册 loader 通知 | IAT、`g_real*` |
| `PayloadThread` | `DllMain` | 等 dxgi/窗口、`ov::Init` | 配置全局 |
| 应用渲染线程 | Present 回调 | `OnPresent` → 绘制、首次子类化 | D2D 对象、`g_capture`（不碰）、环形缓冲（读） |
| 应用 UI 线程 | `ViewProc` | hover/press/click → `OnButtonClick` | `g_hover/g_pressed`（`volatile LONG`） |
| `ScriptRunThread` | 每次执行（脚本） | 逐行：门控等待 + 合成动作 | `g_script`（只读局部副本）、**独占 `g_capture`**、`g_scriptActive` |
| `ScriptHotkeyThread` | `Init` | 2s 轮询重载 + 热键 | `g_script`（独占锁写）、状态行 |

保护手段：

- `g_scriptLock`（SRWLOCK）：重载独占写 vs 运行开始时共享拷贝；
- `g_screenLogLock`（SRWLOCK）：日志写入独占 vs 每帧快照共享；
- `InterlockedCompareExchange` 的「单飞」标志：`g_scriptActive`（脚本运行期，原来跟 ini 链共用的 `g_chainActive` 已随链一起删除）；
- `g_capture` 没有锁 —— **依赖「同一时刻只有一个 run 在跑」这个不变式**（唯一的并发者是 `WaitForCreamAt`，它只在 `ScriptRunThread` 里被调），`g_hover/g_pressed` 是跨线程裸 `volatile LONG`（仅用于外观，无一致性要求）。

### parity 常量（改 DLL 就得改编辑器）

`../script_editor.py` 顶部有一组带「改一边必须改另一边」注释的常量，与 DLL 严格对应：

| 编辑器 | DLL 位置 | 值 |
|---|---|---|
| `GATE_POLL_MS` | `g_gatePollMs`（ini `gate_poll_ms`）/ `<30→120` | 120 |
| `GATE_STABLE` | `WaitForCreamAt` 的 `>= 3` | 3 |
| `GATE_MAX_MS` | `WaitForCreamAt` 在 `maxMs<=0` 时的 limit | 4000 |
| `CREAM_MIN_PX` | 只有编辑器用：「门控检查（不点）」没填 `wait_px` 时的默认阈值（DLL 侧 `wait_px` 本身就是阈值，没有默认值） | 40 |
| `DBLCLK_GAP_MS` | `SimulateClickKindAt` 的 `Sleep(40)` | 40 |
| `PASTE_FOCUS_MS` | `PasteTextToApp` 的 `Sleep(80)` | 80 |
| `MAX_EVENTS` / `MAX_TEXT` / `MAX_LOOP` | `kScriptMaxEvents` / `kScriptText-1` / loop clamp | 32 / 255 / 99 |
| ±200 × ±40 判定带 + `R>228,G>210,B<228,R-B>22` | `CountCreamBand` | 一致 |
| `static_cast<LONG>(逻辑 × scale)` | 截断，不是四舍五入 | 一致 |

编辑器故意多出的只有两样：「停止」能中止、门控检查（不点）+ 存抓帧。

---

## 11. 已知边界（当前实现的诚实清单）

- **没脚本时按钮真的什么都不做**：两段点击链删除后没有任何兜底；旧 ini 里残留的 `click_x` / `click_y` / `click2_*` / `click_delay_ms` 会被当未知键**静默忽略**（不报错，也不生效）。
- **点击失败的原因行会刷屏**：`verbose` 下每次点击都重打一行原因（这是为了「点了没反应也能看到为什么」而做的取舍）；一直看就是一直刷， 2s 轮询那边仍然只打一次。
- **日志面板现在常驻**：不再淡出，会一直占着左下角（默认 6 行）；想收小调 `log_lines`，想完全关掉调 `log_on_screen=0`（重启生效）。缓冲 24 行，超出显示窗口的旧行只能去文件里看（没有人工翻页）。
- **重复日志靠时戳区分，不靠文本**：面板不做去重/计数合并，相同的行就是占多行；前缀时戳精确到毫秒，连点时看到的就是时戳在跳。想要「一屏看懂发生了多少次」就去翻文件（那里还有 pid/tid）。
- **DPI / 尺寸变化不重算**：`g_scale` 在 `Init` 时定一次，跨屏移动或改缩放后叠加层与脚本坐标都会偏（待办 §7-5）。`HookResize` 只清 RTV/Bitmap 缓存。
- **最小化时仍会进 Present**，没有暂停绘制的逻辑。
- **兜底路径没有文字**：`overlay: using d3d11 fallback (no text)` 出现时，胶囊只剩色块，界面日志和状态行都不画。
- **粘贴要求前台**：`SetForegroundWindow` 受 Windows 焦点限制策略影响，切走了就是 `script: clipboard busy, paste skipped` 或粘到别处。
- **脚本没有中止/分支**：`loop` × 行数跑完为止；条件只有 `wait_px` 这一种「等不等到」，超时也照点。
- **`ScriptReload` 在 `script=` 指空时不降档**（不会去看 `active.txt`），这是刻意的，但容易误解。
- **门控判色是硬阈值**，换主题/换分辨率后可能要重新取样（现在的判式按「奶油色 #FFEB?? 系」实测得来）。
- 配置只在启动时读一次，改 `overlay.ini` 需重启应用。

---

## 12. 构建 / 部署 / 回滚

```bat
:: 构建（VM：VS 2022 Build Tools + MASM）
C:\native\proxy\build.bat

:: 部署（就一个文件）
taskkill /f /im TTVoice.exe 2>nul
copy /y "C:\native\proxy\version.dll" "%APPDATA%\TTVoice\version.dll"
start "" "%APPDATA%\TTVoice\TTVoice.exe"

:: 看日志
type "%APPDATA%\TTVoice\proxy.log"   REM 默认 version.dll 同目录；回退则在 %TEMP%
```

**回滚**：删 `%APPDATA%\TTVoice\version.dll`。安装目录本来没有这个文件，删掉即恢复原样。

运行期文件全集：

| 路径 | 来源 |
|---|---|
| `%APPDATA%\TTVoice\version.dll` | 部署产物 |
| `%APPDATA%\TTVoice\overlay.ini` | 首次运行由 `LoadConfig` 生成的模板；老 ini 缺 `log_*` 键时追加一次（修好 `=` 被改 NUL 的探测 bug 后不再每次启动都追加） |
| `%APPDATA%\TTVoice\scripts\<名称>.json` | `../script_editor.py` 保存 |
| `%APPDATA%\TTVoice\scripts\active.txt` | 编辑器保存时写，DLL 按它选脚本 |
| `%APPDATA%\TTVoice\proxy.log` | DLL 日志（默认 version.dll 同目录；模块路径取不到时回退 `%TEMP%\proxy.log`） |
| `%TEMP%\pplugin_probe_line<N>.bmp` | 编辑器「门控检查」存的抓帧（DLL 不产生） |

---

## 13. 一图总览

```
version.dll（代理）
├─ 转发 17 个 version API ─────────────────→ System32\version.dll
├─ D3D11HookThread：IAT + GetProcAddress ──→ 给 ANGLE 的 device 补 BGRA_SUPPORT
├─ PayloadThread ──→ ov::Init
│                     ├─ LoadConfig(overlay.ini)      ├─ ResolveFontPath
│                     ├─ InstallHooks → PatchVTable   └─ ScriptStartHotkey（2s 轮询/热键）
│
├─ Present hook（应用渲染线程，每帧）
│     ├─ IsTargetSwapChain → EnsureViewSubclass(FLUTTERVIEW)
│     ├─ DrawD2D：胶囊（bg/高亮/描边/文字） + DrawScreenLog（状态行 + 日志镜像）
│     └─ 失败 → DrawFallback（纯色块，无文字）
│
└─ 点按钮（UI 线程 ViewProc 吃下按钮矩形内的点击）
      ├─ ScriptTryRun(true) 成功 ─→ ScriptRunThread：门控 → 滚轮 → 粘贴 → 点击 → delay，×loop
      └─ 失败（没脚本 / 被拒收）→ 什么都不做，但每次点击都重打一行原因（no json found / parse failed）
```

---

## 14. `script` 插件（点击脚本执行器）

`version.dll` 里的脚本引擎焊在叠加层上：要 sideload 代理、要 `InstallHooks` 成功、要人点胶囊。`script` 插件是同一套执行语义的**插件形态**——同一个 JSON、同一批输入动作、同一个门控算法，装进 `plugins\script\` 由框架热加载，不渲染、不装 hook、不建线程（§3.1 契约）。

### 14.1 文件

| 文件 | 职责 |
|---|---|
| [`plugin/script/script_plugin.cpp`](file:///D:/AIProjects/PlusPlugin/plugin/script/script_plugin.cpp) | 插件本体：`GetDesc`/init/tick/shutdown、`script_engine.inc` 需要的同名 shim（`LogFromOverlay`/`SetScriptStatus`/`g_hwnd`/`g_viewHwnd`/`g_scale`/`g_gatePollMs`）、`CaptureView`/`CountCreamBand`、**分步执行器**、热键轮询、`run.txt` 触发 |
| [`plugin/script/plugin.json`](file:///D:/AIProjects/PlusPlugin/plugin/script/plugin.json) | 清单：`name=script`、`dll=script.dll`、`priority=10`、`enabled=true` |
| [`plugin/script/build_plugin.bat`](file:///D:/AIProjects/PlusPlugin/plugin/script/build_plugin.bat) | VM 侧编译（`cl /LD /MT /O2 /EHsc /W4 /utf-8`） |
| [`build_plugin.py`](file:///D:/AIProjects/PlusPlugin/build_plugin.py) | 通用插件流水线：建 `C:\native` → 上传 → 编译 → 回填项目 → 装到 `%APPDATA%\TTVoice\plugins\<name>\` |
| 产物 `plugin/script/script.dll` | x64 单文件 |

引擎仍是**同一份源码**：`script_plugin.cpp` 直接 `#include "../../proxy/script_engine.inc"`（`overlay.inc` 在 include 之前提供的东西，这里由插件的 shim 提供同名同义的一份）。所以 §8 的 JSON 契约、§10 的 parity 常量表对它逐条成立，改引擎两边同步生效。引擎里「起线程跑整份脚本」和「`RegisterHotKey` 热键线程」这两半被**刻意不使用**（编译器会给 C4505，文件里已用 pragma 局部关掉）。

### 14.2 生命周期与线程归属

```
PayloadThread（框架）                 PluginControlThread（框架，每 16ms）
  plugins::Start()                      tick()
    └─ ScanPlugins()                      ├─ 每 1s：ScriptReload(false)（stat 一次，静默）
         └─ LoadPlugin("script")          ├─ 每 tick：轮询 hotkey（GetAsyncKeyState 边沿）
              ├─ LoadLibraryExW           └─ 每 300ms：看 scripts\run.txt
              ├─ GetDesc()
              └─ init(host)              RunPendingTasks()
                   ├─ LoadConfig()           └─ RunStep()：延迟/门控/动作，各一步
                   ├─ RefreshView()               └─ 自己 submitTask 再排下一步
                   └─ ScriptReload(true)
```

- **init 不建窗口、不建线程**：只存 `host`、读 ini、找 `FLUTTERVIEW`、解析一次脚本、打一行 `script: init abi=... scale=... view=found events=... loop=...`。
- **执行 = submitTask 分步推进**：`kStageDelay` / `kStageGate` / `kStageAct` 三态，每次回调只做一步再把自己重新入队。所以即使门控要等 4 秒，也只是每 `gate_poll_ms` 抓一帧，`tick`、热重载、console 面板泵消息都不被阻塞（这正是 `plugin-framework.md` §8 计划里的「`tick` 里轮询 / `submitTask` 投递执行」）。
- **卸载天然安全**：没有插件线程可 join；`shutdown()` 只把 stage 复位成 idle（丢弃后续步骤）、放掉 DIB/DC、清 host 指针。框架的 `ClearRegistrations` 已先吊销排队任务。
- `g_scale` 取 `GetDpiForWindow(FLUTTERVIEW)/96`，取不到退 `host->scale()`；`host->viewWindow()` 目前还返回主窗口（M1 才记真实子窗口），所以子窗口由插件 `EnumChildWindows` 自己找。

### 14.3 触发方式

| 触发 | 说明 |
|---|---|
| JSON 里的 `hotkey` | `tick` 里 `GetAsyncKeyState` 边沿检测 + 去抖。**不是** `RegisterHotKey`（框架的 `addHotkey` 目前只登记、不派发，等 M1），所以这个键应用自己也会收到 |
| `%APPDATA%\TTVoice\scripts\run.txt` | 文件出现即跑当前选中的脚本，**先删再跑**（崩了不会重放）。脚本/测试脚本用这个，不占键 |

选择脚本的规则与代理完全一致（§8.1）：`overlay.ini` 的 `script=` > `scripts\active.txt` > 最新的 `*.json`；ini 里只认 `script=` 和 `gate_poll_ms` 两个键。

### 14.4 与 `version.dll` 内置引擎的差异

| 方面 | version.dll（§7/§8） | script 插件 |
|---|---|---|
| 加载 | sideload 代理，进程启动期 | 框架扫描 `plugins\` 后 `LoadLibraryEx` |
| 触发 | 胶囊点击 / 自有热键线程 | hotkey 轮询 / `run.txt` |
| 绘制、屏上日志、胶囊状态行 | 有 | 无（`SetScriptStatus` 只记 code + 原因写日志） |
| 执行线程 | 自建 `ScriptRunThread` | 框架线程上的分步 task |
| 门控等待 | `Sleep` 轮询（阻塞该线程） | 每步一帧（不阻塞） |
| 日志 | `%APPDATA%\TTVoice\proxy.log` | 同一条日志（走 `host->log`）：`init`/`shutdown` 带 `[plugin:script]`，`tick`/task 带 `[plugin:framework]`（框架只在 init/shutdown 期设置 `g_activeSlot`），正文一律以 `script: ` 开头 |
| 中止 | 没有 | 没有（编辑器才有停止），但卸载插件即可丢弃后续步骤 |

### 14.5 构建 / 安装

```bat
:: 编译 + 回填项目 + 装到 %APPDATA%\TTVoice\plugins\script\（应用在跑会走热重载）
python build_plugin.py script
python build_plugin.py console          :: 同一脚本也支持 console
python build_plugin.py script console   :: 依次编
```

**全新插件目录不会被运行中的框架发现**（`ScanPlugins` 只在 `Start()` 跑一次，之后只有热重载已扫描到的槽位），所以第一次装上 `script` 后需要重启一次 pplugin；之后改代码重编，`build_plugin.py` 会用 `enabled=false → 覆盖 → enabled=true` 让框架自动加载新版本。

### 14.6 已知边界

- **两套引擎会同时响应同一份 JSON**：代理的胶囊/热键（`RegisterHotKey`）和本插件的 hotkey 轮询都会触发；`plugin_overlay` 迁移完成前，要么 JSON 的 `hotkey` 留空、只用 `run.txt` 触发插件，要么只用胶囊。日志里两边的行都能看到（`[plugin:script]` 前缀可区分）。
- **热键抢不到**：轮询实现意味着按键仍会送进应用；`addHotkey` 派发落地后应改成真正的全局热键。
- **门控是硬阈值**（§11 同一条）：±200×±40 物理 px 带 + `R>228,G>210,B<228,R-B>22`，换主题/分辨率要重新取样。
- **DPI**：`g_scale` 在 `RefreshView` 时刷新（每次 run 前都刷），但一次 run 中途不重算。
- 粘贴仍要求 pplugin 在前台（`SetForegroundWindow` 受焦点策略限制），与代理一模一样。

