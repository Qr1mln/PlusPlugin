<div align="center">

# PlusPlugin

**给你的 Windows 应用装上一只可编程的手：DLL 代理 + 进程内 WebSocket + 浏览器脚本编辑器**

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D6)](https://www.microsoft.com/windows)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C)](https://isocpp.org/)
[![Vue](https://img.shields.io/badge/Vue-3-42B883)](https://vuejs.org/)
[![Python](https://img.shields.io/badge/Python-3.12-3776AB)](https://www.python.org/)

</div>

---

## 这是什么

PlusPlugin 是一个 **Windows 桌面应用的插件框架**。它用同名 DLL 代理（DLL proxy / side-loading）进入宿主进程，在进程内加载一个核心模块 `framework.dll`，由它起一个只监听本机的 WebSocket + HTTP 服务。于是浏览器里的编辑器可以：

- **遥控输入**：鼠标点击/移动/按下抬起/滚轮、按键、UTF-8 文本，全部通过 `PostMessage` 投递到目标窗口 —— 不移动真实鼠标、不抢焦点；
- **取点**：在目标窗口上实时显示光标坐标，或开启拾取模式（左键吞掉、右键取消、宿主侧遮罩显示十字线与实时坐标）；
- **编排脚本**：把上述动作存成 JSON 脚本，一键试跑、逐条执行、单步调试，全程有日志。

> 框架本身不认识任何业务。目标应用、窗口、要做什么，都由脚本和编辑器决定。

## 特性

| | |
|---|---|
| **无侵入注入** | `version.dll` 代理 + 进程内 `LoadLibrary`，不改宿主二进制、不打补丁 |
| **热加载** | 替换 `framework.dll`（或其他模块）即生效，约 1 秒，不用重启宿主 |
| **后台输入** | `PostMessage` 直投 `FLUTTERVIEW` 子窗口，不移动物理光标、不抢焦点 |
| **取点与遮罩** | 30 Hz 光标流推送；拾取时盖一层透明遮罩，画十字线 + 逻辑/物理坐标，可顺手把目标窗口置顶 |
| **脚本编辑器** | Vue 3 网页编辑器：事件表格、行内试跑、随机文本列表、每条操作都有日志 |
| **纯本地** | 服务只绑 `127.0.0.1:47801`，没有外网依赖，没有遥测 |
| **模块化** | 一个"模块" = 导出一个 `NotifyPlatform` 的 DLL，契约全是 JSON，无共享结构体/ABI 校验 |

## 架构

```
editor2（浏览器）
  │  WS JSON + HTTP            ws://127.0.0.1:47801/ws
  ▼
TTVoice.exe 宿主进程
├─ version.dll (proxy)   系统加载的代理：bootstrap / module_loader（热加载）/ protocol（宿主能力 JSON）
└─ framework.dll         独立 DLL，proxy 运行期加载：ws_server · command_handler ·
                         input_dispatcher · window_target · script_store · picker · pick_overlay
       │ PostMessage
       ▼
   Flutter 窗口（FLUTTERVIEW）
```

- 两个 DLL 是**互不包含**的独立模块：`version.dll` 是系统加载的代理入口，`framework.dll` 是它在运行期 `LoadLibrary` 起来的核心模块；
- `version.dll` 提供宿主能力（`mainWindow` / `viewWindow` / `scale` / `refreshWindow`），负责加载模块并监视文件变化实现热加载；
- `framework.dll` 对外提供全部功能：WS/HTTP 服务、命令执行、脚本读写、取点、遮罩；
- 两者共用 `common/log`，写同一份 `%APPDATA%\TTVoice\proxy.log`。

完整的分层图、模块职责、WS / HTTP 协议见 **[docs/架构图.md](docs/架构图.md)**。

## 快速开始

### 1. 环境要求

| 用途 | 需要 |
|---|---|
| 构建（本机） | Python 3.12+、`pip install paramiko`；Node 18+（前端） |
| 构建（交叉编译） | 一台 Linux 机器装 `g++-mingw-w64-x86-64`（构建脚本通过 SSH 远端编译，因此 **Windows 上不需要 MSVC**） |
| 运行 | Windows 10/11 x64，目标应用（默认 `TTVoice.exe`） |

编译机地址、账号写在 `scripts/*_build.py` 顶部的配置区（支持 `~/.ssh/id_ed25519` 密钥或 `VM_PASSWORD` 环境变量）。

### 2. 编译

```bat
python scripts\proxy_build.py                  :: version.dll  -> proxy\version.dll
python scripts\framework_build.py              :: framework.dll -> framework\framework.dll
cd editor2 && npm install && npm run build     :: 前端 -> editor2\dist
```

### 3. 部署

```bat
python deploy.py              :: version.dll（需重启宿主）
python deploy_plugin.py       :: framework.dll + editor2\dist（热加载，约 1s）
```

`version.dll` 与 `version.ini` 放到宿主 exe 同目录（`%APPDATA%\TTVoice\`）。最小配置：

```ini
[host]
target=TTVoice.exe
debug=false

[modules]
; 额外模块的裸 dll 名，一行一个；没有就留空
```

`[host] target` 是唯一必填项（代理只在匹配的进程里活动）；`[modules]` 里的裸 dll 名是额外模块，核心 `framework.dll` 总是加载。

### 4. 打开编辑器

- 生产：构建产物由 framework 直接托管 → 浏览器打开 `http://127.0.0.1:47801/`
- 开发：`cd editor2 && npm run dev`，用 Vite 代理转发到 47801

## 写第一条脚本

1. 「新建」命名脚本 → 表格为空；
2. 「添加鼠标」→ 点「左键拾取」（可勾选"拾取时置顶"）→ 在目标窗口上点一下，坐标自动回填；
3. 「添加延迟」「添加按键」「添加输入」（输入支持**随机列表**：一行一条，执行时随机取一条）；
4. 「试跑」逐条执行并回日志；也可以点某一行的「试跑」只跑那一条；
5. 「保存」写入 `%APPDATA%\TTVoice\scripts\<名字>.json`。

每一步（成功/失败/跳过/取消）都会在下方日志面板留一条记录。

## 目录结构

```
PlusPlugin/
├─ proxy/              version.dll：DLL 代理入口、模块加载/热加载、宿主能力 JSON 协议
├─ framework/          framework.dll：WS/HTTP、命令执行、取点、遮罩、脚本存储
├─ common/             共享 JSON 解析与日志（proxy / framework 各编一份）
├─ libs/civetweb/      内嵌 CivetWeb（C，HTTP + WebSocket）
├─ editor2/            浏览器编辑器（Vue 3 + Vite）
├─ scripts/            构建脚本：SSH → Linux mingw 交叉编译
├─ deploy.py           version.dll 部署（重启宿主）
├─ deploy_plugin.py    framework.dll + 前端部署（热加载）
└─ docs/               架构、协议、交互示意
```

## 常见问题

**改了代码要重启宿主吗？**
只有 `version.dll` 需要（它是被系统加载的代理）。`framework.dll` 和 `[modules]` 里的模块由 proxy 每 700 ms 监视文件标识，替换文件后约 1 秒自动 `shutdown → FreeLibrary → 重新加载`，编辑器 WS 会自动重连。

**为什么用 `PostMessage` 而不是 `SendInput`？**
`SendInput` 会移动真实光标、抢焦点，无法在后台跑。`PostMessage` 只往目标窗口的消息队列投递，代价是个别自绘/Raw Input 程序可能不响应。

**端口被占用？**
服务固定 `127.0.0.1:47801`，CivetWeb 开了 `SO_REUSEADDR`，热加载后可立即重绑。占用排查：`netstat -ano | findstr 47801`。

**日志在哪？**
`%APPDATA%\TTVoice\proxy.log`，格式 `[HH:MM:SS.mmm]-[proxy|framework] 正文`，落盘为本地 ANSI（GBK），记事本打开不乱码。编辑器里另有一份内存操作日志（最多 500 行）。

**杀软/EDR 报毒？**
DLL 代理注入本身就是敏感手法。本项目全部源码可自审、可自编译，不存在下载执行；请只在你拥有或有权控制的应用上使用。

## 已知限制

- 只支持 Windows；`version.dll` 必须与宿主 exe 同目录（系统按 exe 目录搜索同名 DLL）；
- 事件类型里「找图」目前只有数据结构，执行尚未接入（没有截图/模板匹配管线）；
- 脚本可在 framework.dll 内按计划任务执行：支持循环次数、暂停/继续、停止，`delay` 可中断；浏览器关掉也继续跑（状态经 WS `task` 命令或 HTTP `GET /api/tasks` 查询，前端默认每 1.5s 轮询）；
- 单任务串行：同一时刻只能有一个活跃任务，启动新任务前需先停止当前任务；
- WS/HTTP 无鉴权，仅绑定回环地址，不要改成对外监听。

## 参与贡献

欢迎 Issue / PR。提交前请确保：`proxy_build.py`、`framework_build.py` 能编过，`editor2` 能 `npm run build`；架构或协议有变化时同步更新 `docs/`。

## 许可证

[Apache-2.0](LICENSE)
