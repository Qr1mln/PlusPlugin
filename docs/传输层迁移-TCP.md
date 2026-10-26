# 传输层迁移：命名管道 → TCP loopback

> 记录时间：2026-10-26。状态：**迁移已完成并编译部署**（`script_plugin.cpp`、`../script_editor.py`、`pipe_probe.py` 与两处构建脚本均已改为 TCP）。已在 Linux VM 上编译出 dll 并部署到本机应用，应用日志确认引擎已监听 `127.0.0.1:47800`；编辑器侧的端到端试跑待手动验证（构建机沙箱屏蔽 loopback 连接，无法在自动化里直连）。

## 1. 结论

`../script_editor.py` ↔ `script-engine.dll` 的通信通道从命名管道 `\\.\pipe\script-engine` 换成 **TCP loopback `127.0.0.1:47800`**。

- 帧格式**不变**：4 字节小端长度 + UTF-8 JSON，双向。
  - 编辑器 → DLL：`{"cmd":"load","loop":N,"events":[...]}`、`{"cmd":"play"}`、`{"cmd":"stop"}`
  - DLL → 编辑器：`{"t":"log","msg":"..."}`、`{"t":"done"}`、`{"t":"stopped"}`
- 日志前缀 `pipe:` 改为 `sock:`。
- 端口固定 47800（`bind` 失败时打日志，暂不做自动换端口）。

## 2. 为什么放弃管道

现象：编辑器点「运行」后，宿主应用内**没有任何鼠标事件被触发**，编辑器日志停在 `pipe: client connected`。

`%APPDATA%\TTVoice\proxy.log`（已重新编译的 dll）证据：

```
21:42:10.074  pipe: client connected
21:42:40.075  pipe: read timeout        ← PipeReadExact 等 4 字节头部等了 30 秒
21:42:40.076  pipe: client disconnected
```

即 DLL 侧确实在等数据，**一个字节都没收到** ⇒ 编辑器侧没写出去（首要嫌疑：Python ctypes 调 `CreateFileW`/`WriteFile` 未设 `argtypes/restype`，句柄被截成 32 位）。早一轮旧 dll 的表现一致：`21:35:00 client connected` → `21:35:36 disconnected`，同样是「连接成功、零字节到达」。

管道这一层的坑反复出现（异步句柄 + 阻塞读写 + 单实例独占 + ctypes 句柄语义），因此不再纠缠，换 TCP：没有句柄/OVERLAPPED 概念，可直接用 `nc`/`telnet`/python 一行命令验证链路。

> 说明：上一轮已给 `script_plugin.cpp` 补了 `PipeReadExact()`（OVERLAPPED 读满、超时/失败打日志）和 `PipeWaitClient()`（正确处理 `ERROR_PIPE_CONNECTED` / `ERROR_IO_PENDING`，原来会 `continue` 死循环空转）。这两处随 `PipeThread` 一起在 TCP 迁移时删除。

## 3. 待办清单（已全部完成）

### 3.1 `plugin/script-engine/script_plugin.cpp`

1. `#include <winsock2.h>` 必须放在 `<windows.h>` **之前**（避免 `winsock.h` 冲突）。
2. `g_client`（约 431 行，`HANDLE`）换成 `SOCKET`；`WriteClientFrame` / `SendFrameToClient`（约 453-476 行）改为 `send()`，配 `TCP_NODELAY`、`SO_SNDTIMEO=1000`，发送失败静默丢弃。
3. 用 `SocketThread` 替换 `PipeThread`（约 731 行）：
   - `WSAStartup(MAKEWORD(2,2))` → `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`
   - `SO_REUSEADDR` → `bind(127.0.0.1:47800)` → `listen(2)`
   - 外层 `select(200ms)` 轮询 `g_quit`，就绪则 `accept`（单客户端）
   - 内层读循环改用 `recv` 版 `RecvExact`，解析后仍投现有 `Command` 队列 + `SetEvent(g_cmd_ev)`（`ExecThread` 不动）
   - 退出：`closesocket` + `WSACleanup`
4. 初始化处（约 816 行）`g_client = INVALID_HANDLE_VALUE` 改为 `INVALID_SOCKET`；文件头注释里「命名管道」改为「TCP」。

### 3.2 链接

两处构建都要加 `-lws2_32`：

- `build_plugin.py` → `build_on_linux()` 里的 mingw 命令行
- `build_linux.sh` → `build_script()` 的 `-luser32 -lgdi32`

### 3.3 `../script_editor.py`

- `PipeClient` → `SocketClient`：`socket.create_connection(("127.0.0.1", 47800), timeout=2)`，发送 `sendall`，`recv_frame` 循环收满头部/体。
- `close()` 先 `shutdown(SHUT_RDWR)` 再 `close()`：否则 `_pipe_reader` 线程持着 `rlock` 阻塞在 `recv`，`close()` 拿锁会死锁。
- 常量 `PIPE_NAME` 换成 `ENGINE_HOST` / `ENGINE_PORT`。
- 文案：`pipe: 目标管道 ...` → `sock: 目标 127.0.0.1:47800`；`连不上应用管道` → `连不上引擎端口 127.0.0.1:47800`；`pipe: 已发送脚本`、`pipe: → load 发送=`、`pipe: 执行超时`、`pipe: 连接已断开` 统一改 `sock:`。
- `self.pipe` 属性名可保留，减少改动面。

### 3.4 `pipe_probe.py`

改成 TCP 探针（同帧格式、发 `load` + `play`、打印每帧回包），也顺便解决原文件「需先关掉编辑器独占管道」的限制。

## 4. 构建 / 部署 / 验证

本机没有 MSVC（`where cl`、VS2022 均无），编译在 Linux VM 上 mingw 交叉编译：

```bat
python build_plugin.py script-engine
```

流程：SSH 到 `192.168.167.128`（用户 `n111`）上传源码 → `x86_64-w64-mingw32-g++` 编译 → 下载 `plugin/script-engine/script-engine.dll` → `taskkill` TTVoice.exe / 关掉 script_editor.py → 覆盖 `%APPDATA%\TTVoice\plugins\script-engine\script-engine.dll` → 重启 TTVoice。

**改完 dll 必须重启 TTVoice**（插件已加载，不重启跑的还是旧代码）。

验证成功时编辑器日志应依次出现：

```
sock: frame N bytes
sock: recv cmd=load
loaded 5 events, loop=1
sock: recv cmd=play
exec: dispatch play
run: view=0x...
run: events=5 loop=1
mouse: left click (24,1080) rc=1
done
```

## 5. 点击无效的根因（已修，2026-10-26 验证通过）

TCP 打通后日志已能看到 `mouse: left click (36,1620) rc=0`，但界面仍无反应。根因在 `version.dll`：

- `framework/plugin_api.inc` 的 `HostViewWindow()` 一直返回 `g_mainHwnd`（顶层 `FLUTTER_RUNNER_WIN32_WINDOW`）。
- Flutter 真正处理鼠标输入的是它的子窗口 **`FLUTTERVIEW`**，`PostMessage(WM_LBUTTONDOWN)` 投给顶层窗口没人处理 → `rc=0`（投递成功）但界面无任何反应。
- 老的 `view_input.inc` 那套子类化代码在 M1 迁移时被删了，所以框架里再没人找过 `FLUTTERVIEW`。

修复：

1. `framework/plugin_api.inc` 新增 `ResolveViewWindow()`：`EnumChildWindows` 按类名 `FLUTTERVIEW` 查找（优先可见的），结果缓存、`IsWindow` 失效后重找，找不到才退回主窗口；解析成功打 `host view=0x... (FLUTTERVIEW)`。`HostViewWindow()` 返回它。
2. `plugin/script-engine/script_plugin.cpp` 的 `run: view=...` 日志追加窗口类名，便于确认拿到的是 `FLUTTERVIEW` 而不是顶层窗口。

注意：**`version.dll` 与插件 dll 必须同时重新编译部署**（`python build.py` + `python deploy.py` + `python build_plugin.py script-engine`），只更新插件仍然是旧句柄。

## 6. 代码结构（2026-10-26 重构）

原来的单文件 854 行拆成 4 个实现 + 1 个共享头（`plugin/script-engine/`）：

| 文件 | 行数 | 职责 |
|---|---|---|
| `engine.h` | 84 | 内部共享声明：`Event`/`Command`/`CmdType`、`se::` 引擎接口、`tcp::` 接口、`PP_*` 原型 |
| `engine.cpp` | 266 | 命令队列 + 执行线程 + 日志队列/日志线程 + `RunEvent`/`RunLoop` |
| `tcp_server.cpp` | 224 | TCP 服务（127.0.0.1:47800，单客户端）+ 协议解析（nlohmann/json） |
| `input_win32.cpp` | 230 | `PP_*` 输入原语（PostMessage 合成鼠标/键盘）+ 键名/组合键解析 |
| `script_plugin.cpp` | 81 | 插件入口：`GetDesc`、init/tick/shutdown、崩溃捕获 |

依赖与构建：

- **nlohmann/json**（v3.11.3 单头文件，MIT）vendor 在 `third_party/nlohmann/json.hpp`，源码里按 `#include "../../third_party/nlohmann/json.hpp"` 相对包含。`build_plugin.py` 的 `build_on_linux()` 会把整个 `third_party/` 上传到编译机并保持同样层级。
- 线程/队列全部用 **std::thread + std::mutex + std::condition_variable + std::atomic**，不再有 `CreateThread`/`CreateEvent`/`CRITICAL_SECTION`/`WaitForSingleObject` 手工管道。
- 两处构建都加 `-std=c++17`；script-engine 需要 `-lws2_32`（`build_plugin.py` 与 `build_linux.sh` 已加）。`build_linux.sh` 的 script-engine 目标已改为编译 `plugin/script-engine/*.cpp` 全部源文件。
- 不再手写 JSON 解析器（原 `namespace mj` 约 145 行已删），浮点坐标（如 `504.7`）由 nlohmann 直接转 int。

验证：`python build_plugin.py script-engine` 远端 mingw 编译通过（`-Wall` 无告警），部署后日志显示 `plugin: loaded script-engine v0.3.0`、`script-engine: 进程内引擎已启动（TCP 127.0.0.1:47800）`、`host view=0x... (FLUTTERVIEW)`。
