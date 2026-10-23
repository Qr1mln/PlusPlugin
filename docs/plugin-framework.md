# TTVoice 插件框架设计文档

> 本文件描述 `native/version_proxy` 下的插件框架架构，是被 `sdk/ttplugin.h` 引用的权威说明。
> 设计目标：在**无源码**条件下，把原来写死在 `dllmain.cpp` 里的"叠加层 + 点击脚本 + 界面日志"
> 改造成**可热加载/热卸载的插件**，让 `version.dll` 代理本身只做"转发 + 引导 + 持有 hook"。

## 状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | 框架骨架：ABI 契约 / 能力表 / 扫描-加载-卸载-控制 / 日志分发 | ✅ 已落地（`framework/`、`sdk/`、`plugin_example/`、`plugin_console/`） |
| M1 | 渲染派发层 + overlay 功能迁移为插件 | 🟡 规划中（`framework/render.inc` 未写；`overlay.inc`/`script_engine.inc` 待迁移） |

---

## 1. 总体架构

```
                TTVoice.exe
                     │ 静态导入 version.dll（17 个导出，全按名字）
                     ▼
   ┌─────────────────────────────────────────────┐
   │  version.dll 代理  (dllmain.cpp + stubs.asm)  │  ← 转发全部导出到 System32\version.dll
   │   · 解析真身（DllMain 内）                      │
   │   · 早期起 D3D11 / GetProcAddress hook 线程     │  ← framework 拥有，插件绝不可自己装
   │   · PayloadThread：等 dxgi + 主窗口 → plugins::Start() │
   └───────────────────────┬─────────────────────┘
                            │ 调用
                            ▼
   ┌─────────────────────────────────────────────┐
   │  framework（同一翻译单元内，.inc 拼进 dllmain）│
   │   · plugin_api.inc    —— TTPluginHost 能力表  │
   │   · plugin_loader.inc —— 生命周期 / 控制       │
   │   · render.inc [M1]   —— Present hook + 派发  │  ← 目前 hook 仍暂留在 overlay.inc
   └───────────────────────┬─────────────────────┘
                            │ 加载 / 调度
                            ▼
   ┌─────────────────────────────────────────────┐
   │  plugins（%APPDATA%\TTVoice\plugins\<NNN-name>）│
   │   · 000-empty   （empty_plugin.cpp，验 M0 通路）│
   │   · 010-console （log_console.cpp，日志窗口）   │
   │   · [M1] plugin_overlay/（按钮+脚本+屏上日志）  │  ← 由 overlay.inc 迁移而来
   └─────────────────────────────────────────────┘
```

**核心原则（贯穿整个框架）**：framework 拥有每一条 hook、每一个系统注册点、每一个线程。
插件只通过 `TTPluginHost` 这张表**预订回调**，因此一次卸载可以把整组预订从一处吊销，再 `FreeLibrary`。
插件自己绝不允许安装 hook、创建线程或触碰系统级注册点。

---

## 2. 分层与文件

| 路径 | 职责 |
|---|---|
| `dllmain.cpp` | 代理载荷：转发 17 个导出 → 起 PayloadThread；`DllMain` 内解析真身 + 起早期 D3D11 hook 线程。M1 完成后只保留"代理 + 引导 + 启动框架"。 |
| `stubs.asm` | 17 个 stub：`jmp qword ptr [g_pfn_X]`（AheadLib 风格，因 MSVC `.def` 转发不可用）。 |
| `exports.def` / `proxy_exports.h` | 导出 17 个 stub 名（`LIBRARY version`）/ X-macro 列表（定义与解析指针共用）。 |
| `sdk/ttplugin.h` | **ABI 契约**：插件与框架的共同语言，纯 ASCII、POD、`extern "C"`。 |
| `framework/plugin_api.inc` | host 侧实现 `TTPluginHost` 能力表、日志扇出、吊销、生命周期状态。 |
| `framework/plugin_loader.inc` | 扫描 / 校验 / 加载 / 卸载 / 任务派发 / `control.txt` 控制；单一 loader 线程。 |
| `framework/render.inc` **[M1]** | Present hook + 每帧遍历 `draws[]` 调 `addDraw` + 输入子类化后按 `regions[]` 命中测试派发点击。当前未编写，hook 暂留 `overlay.inc`。 |
| `plugin_example/empty_plugin.cpp` | 最小插件，验证 M0 加载/卸载。 |
| `plugin_console/log_console.cpp` | 日志窗口插件，验证 `addLogSink` + `tick` 消息泵。 |
| `plugin_overlay/` **[M1]** | 由 `overlay.inc`+`script_engine.inc` 迁移而来的产品插件（按钮 / 点击脚本 / 屏上日志）。 |

---

## 3. ABI 契约（`sdk/ttplugin.h`）

`TTPLUGIN_ABI = 1`。契约约束（审查强制）：

1. **POD only，C 兼容**：不能用 C++ 类型、STL、异常。
2. **字段只增不改**：永远不重排、不改语义；删除的字段保留为 reserved 指针，只有破坏性变更才 bump abi。
3. **插件不持有系统资源**：不得自己装 hook / 建线程 / 碰系统级注册点，一切走 `TTPluginHost`，便于卸载时吊销。
4. **移除字段保留为 reserved 指针**；abi 只在破坏性变更时 +1。
5. **文件保持纯 ASCII**。

### 3.1 `TTPluginDesc`（插件清单，从 `TTPlugin_GetDesc()` 返回）

| 字段 | 含义 |
|---|---|
| `abi` | 插件要求的最低框架 abi（通常 `TTPLUGIN_ABI`） |
| `structSize` | 插件侧 `sizeof(TTPluginDesc)`；框架要求 `>=` 自身大小才接受 |
| `name` | ASCII 稳定 id，≤63 字符 |
| `version` | ASCII 自由文本，≤31 字符 |
| `init(host)` | 加载时调用一次；返回 `TTPLUGIN_OK` 接受，其他值框架卸载 DLL |
| `tick()` | 可选；框架线程周期调用，必须快返回（阻塞会拖慢所有插件） |
| `shutdown()` | 卸载前调用一次；在此释放图形/COM 资源、join 线程（**不要**在 `DllMain` 做） |

### 3.2 `TTPluginHost`（能力表，框架实现，init 时交给插件）

| 字段 | 作用 |
|---|---|
| `abi` / `structSize` | 校验用：插件比对 `abi == TTPLUGIN_ABI` 且 `structSize >= sizeof(TTPluginHost)` |
| `log(text)` | 中心日志，UTF-8 单行；非空 |
| `pluginDir()` | 插件目录绝对 UTF-8 路径（定位自身数据文件） |
| `mainWindow()` / `viewWindow()` | 主窗口 / 接收输入的子窗口句柄（UI 就绪前可能为空） |
| `scale()` | 窗口 DPI 缩放（96 dpi 时为 1.0） |
| `addDraw(fn, priority, user)` | 注册绘制回调（逻辑像素坐标，框架套用 scale）；priority 小者先画 |
| `addRegion(rect, priority, fn, user)` | 注册可点击矩形（逻辑像素）；priority 高者赢得命中测试 |
| `addHotkey(vk, fn, user)` | 注册全局热键（框架拥有 `RegisterHotKey`） |
| `submitTask(fn, user)` | 向框架线程投送任务（队列满返回 `TTPLUGIN_ERR_FULL`） |
| `query(capability)` | 能力协商，目前 reserved，统一返回 null |
| `addLogSink(fn, user)` | 订阅框架日志流（每写一行都投递；每插件一个 sink） |

### 3.3 回调与结果码

```c
typedef void (__cdecl *TTPluginDrawFn)(void* d2dCtx, float width, float height, void* user);
typedef void (__cdecl *TTPluginClickFn)(int kind, float x, float y, void* user); // kind: 0 左 / 1 双击 / 2 右
typedef void (__cdecl *TTPluginHotkeyFn)(int vk, void* user);
typedef void (__cdecl *TTPluginTaskFn)(void* user);
typedef void (__cdecl *TTPluginLogFn)(const char* line, void* user);
```

结果码：`TTPLUGIN_OK=0` / `TTPLUGIN_ERR_FULL=-1` / `TTPLUGIN_ERR_STATE=-2` / `TTPLUGIN_ERR_PARAM=-3`。
容量：`TTPLUGIN_MAX_DRAWS=8` / `TTPLUGIN_MAX_REGIONS=16` / `TTPLUGIN_MAX_HOTKEYS=8`。
插件唯一导出：`TTPlugin_GetDesc`（宏 `TTPLUGIN_ENTRY_NAME`）。

---

## 4. 生命周期（状态机）

```
kEmpty ──scan──▶ kDiscovered ──(enabled=false)──▶ kDisabled
                     │ load
                     ▼
                  kLoading ──init() ok──▶ kActive ──unload──▶ kDraining
                     │                         │                  │ drain ok
                init()!=OK                     │                  ▼
                     ▼                         │             (shutdown ok) ──▶ kUnloaded
                kRejected                  tick/tasks            │ drain timeout
                                              draw/region         ▼
                                              log sink        kQuarantined（保留映射，永不再调用）
```

- **加载协议**：`LoadLibraryExW(path, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | DEFAULT_DIRS)` →
  取 `TTPlugin_GetDesc` → 校验 abi/structSize → 置 `g_activeSlot` 调 `init()` → `kActive`。
- **卸载协议**：置 `kDraining` → `ClearRegistrations` 吊销全部预订 →
  自旋等 `inflight` 归零（超时 `kDrainTimeoutMs=2000ms` → 转 `kQuarantined`，绝不卸载仍在栈上的代码）→
  调 `shutdown()` → `FreeLibrary` → `kUnloaded`。
- 全局状态（`g_slots[]`、`g_activeSlot`）只在 loader 线程改动；只有 `PluginSlot::inflight` 经 `Interlocked*` 跨线程共享。

---

## 5. 插件清单（`plugin.json`）

扫描根：`%APPDATA%\TTVoice\plugins\`。每个插件一个目录 `<NNN-name>\`，内含 `plugin.json` + `<dll>`。

```json
{
  "schema": 1,
  "name": "console",
  "version": "0.1.0",
  "enabled": true,
  "priority": 5,
  "dll": "log_console.dll"
}
```

| 字段 | 说明 |
|---|---|
| `schema` | 固定 1 |
| `name` | 插件 id（与目录无关；缺省用目录名推导） |
| `version` | 自由文本 |
| `enabled` | `false` 时标 `kDisabled` 不加载（目录名以 `_` 开头同样禁用，扫描阶段跳过） |
| `priority` | 加载/调度优先级 |
| `dll` | 相对目录的 DLL 名；`IsSafeRelativeName` 拒绝绝对路径/`..`/通配符，防越目录加载 |
| `sha256` | **现状**：M3 才强制信任链，目前仅容忍并打日志"has no sha256 (trust check arrives in M3)" |

目录按 `FindFirstFile` 收集后**排序**，`NNN-` 前缀保证加载顺序可复现（`000-empty` → `010-console`）。

---

## 6. 控制接口（`plugins\control.txt`）

loader 线程每 `kControlPollMs=500ms` 轮询。命令执行前文件被**截断**，避免中途崩溃重放。

| 命令 | 作用 |
|---|---|
| `load <name>` | 加载指定插件 |
| `unload <name>` | 卸载指定插件（走上面卸载协议） |
| `list` | 打印所有插件状态 |

---

## 7. 编写插件（作者指南）

最小骨架（`plugin_example/empty_plugin.cpp`）：

```cpp
#include "../sdk/ttplugin.h"
static const TTPluginHost* g_host = nullptr;
static int __cdecl OnPluginInit(const TTPluginHost* host) {
    g_host = host;
    if (host == nullptr || host->abi != TTPLUGIN_ABI) return TTPLUGIN_ERR_PARAM;
    host->log("empty: init");
    return TTPLUGIN_OK;
}
static int __cdecl OnPluginTick(void) { return 0; }
static int __cdecl OnPluginShutdown(void) {
    if (g_host && g_host->log) g_host->log("empty: shutdown");
    return TTPLUGIN_OK;
}
static const TTPluginDesc kDesc = {
    TTPLUGIN_ABI, sizeof(TTPluginDesc), "empty", "0.1.0",
    &OnPluginInit, &OnPluginTick, &OnPluginShutdown
};
extern "C" __declspec(dllexport) const TTPluginDesc* __cdecl TTPlugin_GetDesc(void) { return &kDesc; }
BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID) { (void)m; (void)r; return TRUE; } // 必须空
```

要点（来自 `plugin_console/log_console.cpp` 的踩坑）：

- **`DllMain` 必须为空**：所有资源在 `init` 建、`shutdown` 放。
- **取自身模块句柄用 `GetModuleHandleExW(UNCHANGED_REFCOUNT | FROM_ADDRESS, &SomeLocalFn, &mod)`**：
  普通 `GetModuleHandleEx` 会 +1 引用，导致框架 `FreeLibrary` 永远卸不掉你。
- **需要窗口就用 `tick` 泵消息**：框架线程就是你的 UI 线程，`PeekMessage/DispatchMessage` 放进 `tick()`。
- **日志 sink 可跑在任意线程**：只往环形缓冲写 + `PostMessage` 唤醒，绝不直接碰窗口。
- **不要自己建线程 / 装 hook**：这些是框架的职责。
- 只依赖 `user32.lib` / `gdi32.lib`（系统库，`/MT` 静态链接）。

构建（`build_plugin.bat`）：

```bat
cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 xxx.cpp ^
   /link /OUT:xxx.dll /MACHINE:X64 user32.lib gdi32.lib
```

安装：把 `xxx.dll` + `plugin.json` 复制到 `%APPDATA%\TTVoice\plugins\<NNN-name>\`。

---

## 8. M1 渲染派发层（规划）

**现状缺口**：`plugin_api.inc` 已能存 `draws[]` / `regions[]`，但**没有任何地方调用它们**；
`plugin_loader.inc` 的 `RunPendingTicks` / `RunPendingTasks` 已就位，注释明确写
"draw and region fan-out lands in M1"。而 Present hook、vtable 补丁、D3D11 BGRA hook、
`FLUTTERVIEW` 子类化、区域命中测试目前仍全部写在 `overlay.inc` 里。

**设计（`framework/render.inc`）**：

1. 把 `overlay.inc` 的 `HookPresent/Present1/Resize` + `PatchVTable`、`HookCreateDevice*` +
   `HookGetProcAddress` + `LdrRegisterDllNotification`、`EnsureViewSubclass` / `ViewProc` 收进 framework。
2. 每帧在 Present 里：对 `g_slots[]` 中所有 `kActive` 插件，按 `draw.priority` 升序调用
   `draw.fn(d2dCtx, width, height, draw.user)`；`d2dCtx` 以 `void*` 传（保持 SDK 不含 D2D 头）。
3. 输入：子类化 `viewWindow`，命中测试 `regions[]`（priority 高者胜），在窗口线程调 `region.fn(kind, x, y, user)`。
4. 坐标统一逻辑像素，框架按 `scale()` 套 DPI。

**overlay 功能迁移目标（`plugin_overlay/`）**：

- 按钮绘制 + 屏上日志 → `addDraw`；
- 按钮点击 → `addRegion`（在 `init` 注册顶栏矩形）；
- 屏上日志订阅 → `addLogSink`（复用 `plugin_console` 那套 ring + 绘制逻辑）；
- 点击脚本引擎（`script_engine.inc`）作为插件内模块：`tick` 里轮询 / `submitTask` 投递执行。
- 迁移完成后，`overlay.inc` / `script_engine.inc` 从顶层删除，`dllmain.cpp` 不再 `#include` 它们。

---

## 9. 运行期布局与构建

```
%APPDATA%\TTVoice\
  ├─ version.dll              # 代理产物（部署只需这一个文件，由 build.bat 产出）
  ├─ overlay.ini              # 旧单体路径的按钮配置；M1 后由插件自行管理
  └─ plugins\
       ├─ 000-empty\  {plugin.json, empty.dll}
       ├─ 010-console\{plugin.json, log_console.dll}
       └─ [M1] <NNN>-overlay\{plugin.json, overlay.dll}
```

- `build.bat`：`ml64` 汇编 `stubs.asm` + `cl` 链接产出 `version.dll`。
- 各插件 `build_plugin.bat`：编译并安装到对应 `plugins\<NNN-name>` 目录。
- 回滚：删除 `%APPDATA%\TTVoice\version.dll` 即可（安装目录原本无此文件）。

---

## 10. 已知边界 / 待办

- [ ] **M1 render.inc 未写**，`addDraw`/`addRegion` 暂无人调用；overlay 仍是单体老路径。
- [ ] `overlay.inc`(1566 行) + `script_engine.inc`(832 行) 待迁移为 `plugin_overlay`。
- [ ] `viewWindow()` 当前返回主窗口占位，待 M1 的 Present hook 记录真实子窗口后修正（见 `plugin_api.inc` 注释）。
- [ ] 信任链 `sha256`（plugin.json 已容忍字段，M3 强制）。
- [ ] 发布：DLL / 启动器代码签名、EDR 白名单沟通。
- [ ] 构建需 VS 2022 Build Tools + MASM（本机无 MSVC，编译在 VM 完成）。
