# TTVoice（TT语音）架构分析与健壮性测试报告

| 项目 | 内容 |
|---|---|
| 报告类型 | 同类软件架构分析 / 健壮性测试基线文档 |
| 分析对象 | TTVoice（TT语音）Windows 客户端 v1.22.0+1（server_version 6.88.0） |
| 厂商 | 广州趣丸网络科技有限公司（Quwan） |
| 样本位置 | `C:\Users\111\AppData\Roaming\TTVoice` |
| 分析日期 | 2026-10-22 |
| 分析方式 | 静态文件系统枚举 + 配置/资产清单提取 + 运行时进程/服务/驱动核查（黑盒，未反编译二进制） |
| 用途 | 为公司自研同类产品（工作区 `d:/AIProjects/TTVoice`）提供架构参照与测试输入 |
| 保密级别 | 内部 |

---

## 1. 执行摘要

TTVoice 是一款面向游戏开黑场景的**语音社交桌面客户端**，技术形态为 **Flutter（Dart AOT）+ 自研 C++ 插件体系 + 多家第三方 Native SDK** 的混合架构。主要特征：

1. **多进程模型**：主进程（`TTVoice.exe`，Dart AOT 约 36.5 MB）之外，存在独立 Flutter 运行时子进程（媒体播放器 `tt_media_player.exe`）、Windows 服务（`TTAcceleratorService`）、音效增强进程（FXSound 引擎）及 7 个 Crashpad 崩溃处理进程。
2. **三层 Native 依赖**：ZEGO 实时语音 SDK（RTC）、自研网络加速 SDK（基于 WFP Netfilter + WinPcap + Wintun，含 **9 类内核驱动**）、FXSound 音频增强（含 `fxvad.sys` 内核驱动）。
3. **强自我保护姿态**：DLL SHA-256 清单 + 64 字节签名（Ed25519 特征）+ 密钥文件校验；业务/加速/崩溃配置均加密存储；日志采用腾讯 Mars xlog 加密格式。
4. **重大攻击面集中点**：WebView2 + `prompt()` 型 JS Bridge（历史上同类设计的 RCE 高发区）、内核驱动 IOCTL 面、加速器流量劫持面（自带 CA 证书）、安装目录为用户可写（`%APPDATA%`，DLL 劫持面）。

本报告第 6 章枚举全部攻击面，第 7 章给出可直接执行的健壮性测试矩阵（12 个测试域），作为后续测试用例设计的输入。

---

## 2. 分析范围与方法

| 项 | 说明 |
|---|---|
| 范围 | 安装目录全量文件枚举（约 1300 个文件）、Flutter 资产与 pubspec、运行时进程/服务/驱动、本地数据目录（`%APPDATA%`、`%LOCALAPPDATA%`、`Documents\TTVoice`） |
| 未覆盖 | 服务端协议细节、二进制逆向/反编译、驱动源码级审计、动态插桩（列入后续计划） |
| 工具 | 文件系统枚举、版本信息读取（PowerShell VersionInfo）、服务/驱动查询（`sc query`）、进程清单（`tasklist`）、证书/签名元数据（`certutil`） |

> 说明：本章所有结论均来自上述静态与运行时证据；涉及「推测」处已标注，需在后续测试中验证。

---

## 3. 软件识别与产品定位

| 属性 | 值 | 证据 |
|---|---|---|
| 产品名 | TTVoice / TT语音 | `TTVoice.exe` VersionInfo（FileDescription: TT语音） |
| 版本 | 1.22.0+1（客户端）/ 6.88.0（服务端约定版本） | `pubspec.yaml`、`dll_manifest.json`（version_name 1.22.0） |
| 版权 | ©2014-2025 广州趣丸网络科技有限公司 | `TTVoice.exe` LegalCopyright |
| 产品形态 | 游戏开黑语音 + 组队社交（频道/房间/即时通讯/发帖/小游戏/AI 游戏助手） | `pubspec.yaml` 描述与模块清单 |
| 安装形态 | 绿色化安装于 `%APPDATA%\TTVoice`（用户目录，非 `Program Files`），含 `uninstall.exe` | 目录结构 |
| 技术底座 | Flutter for Windows，Dart SDK `^3.6.0`；AOT 产物 `data/app.so`（36,488,096 B） | `pubspec.yaml`、文件清单 |
| 渲染后端 | 同时携带 GLES（`libEGL/libGLESv2`）与 Vulkan（`vulkan-1.dll`/`vk_swiftshader.dll`） | 文件清单 |

---

## 4. 总体架构

### 4.1 进程模型（运行时实测）

```
┌─────────────────────────────────────────────────────────────────┐
│ 用户会话（当前用户）                                              │
│                                                                  │
│  TTVoice.exe (主进程, ~1.1 GB RSS)                                │
│   ├─ data/app.so                Dart AOT 业务代码                 │
│   ├─ data/flutter_assets/       Flutter 资产 + 35 个自研包 + ~60 模块 │
│   ├─ 插件 DLL 层 (~45 个)        Flutter Windows 插件 (C++)        │
│   ├─ ZegoExpressEngine.dll       RTC 语音引擎 (3.25.0-private.559)│
│   ├─ crashpad_handler.exe ×7    崩溃捕捉 (aa_tt_crash_pad_handler)│
│   └─ TTVoice.exe.WebView2\EBWebView  WebView2 用户数据目录         │
│                                                                  │
│  tt_media_player.exe (独立进程, ~521 MB, 自带 Flutter 运行时)      │
│   └─ %LOCALAPPDATA%\tt_media_player\  独立安装树                  │
│                                                                  │
│  tt_sound_enhancer.exe (音效增强)                                 │
│   └─ %LOCALAPPDATA%\tt_sound_enhancer\engine\  FXSound 引擎        │
│       └─ drivers\win10\x64\fxvad.sys  VAD 降噪内核驱动             │
│                                                                  │
│  tt_accelerator_service.exe (Windows 服务: TTAcceleratorService)  │
│   └─ accel_sdk\sdk_x64\         网络加速 SDK (WFP/WinPcap/Wintun) │
│       └─ 9 类内核驱动 (.sys)                                       │
│                                                                  │
│  report_crash_handler.exe / report_service.exe  崩溃上报           │
│  upgrade.exe / TTVoiceDelayedStart.exe          更新与延迟启动     │
└─────────────────────────────────────────────────────────────────┘
```

**实测进程清单（2026-10-22）**：`TTVoice.exe`、`tt_media_player.exe`、`tt_accelerator_service.ex`（Services 会话 0）、`report_crash_handler.exe`、`crashpad_handler.exe` ×7。

### 4.2 Dart 侧代码组织（Monorepo Workspace）

`pubspec.yaml` 采用 Flutter workspace 单体仓库：

- **基础能力包（35 个 `packages/tt_*`）**：`tt_core`（内核）、`tt_grpc`/`tt_proto`（gRPC+Protobuf）、`tt_kv`（KV 存储）、`tt_common_db`（SQLite）、`tt_module`（模块化框架）、`tt_multi_process`/`tt_hot_key`/`tt_win32_api`/`tt_system`（系统能力）、`tt_js_bridge`/`tt_webview`（WebView 与 JS 桥）、`tt_upgrade`（升级）、`tt_log`/`tt_log_report`/`tt_apm`/`tt_app_track`（日志/监控/埋点）、`tt_oss_obs`（对象存储上传）、`tt_remote_config`（远程配置）等。
- **业务模块（约 60 个 `modules/*/module|export`）**：login、home、im（即时通讯）、channel（频道）、room_card、quick_match（快速匹配）、gamehall、game_mod、game_booster、lite_game、float（悬浮窗）、browser / float_browser（应用内浏览器）、post（帖子）、ai_game_guide（AI 游戏助手）、roco_kingdom、easytier（P2P 组网）、wiki_tool、share、notification_popup 等。
- **架构模式**：BLoC 状态管理（`bloc`/`flutter_bloc`）+ `get_it` 依赖注入 + `go_router`（代码生成路由 `go_router_builder`）+ `freezed`/`json_serializable` 数据类代码生成。

### 4.3 Native 插件层（C++ DLL，约 45 个）

| 类别 | 组件 | 职责 |
|---|---|---|
| 自研 | `tt_system_plugin`、`tt_win32_api_plugin`、`tt_hot_key`、`tt_multi_process_plugin`、`tt_accelerator_v2(_ffi)` | Win32 API、全局热键、多窗口/多进程、加速器 FFI |
| 音频 | `TTAudioEffect.dll`、`audioplayers_windows_plugin`、`flutter_volume_controller`、`NEP2.dll` | 音效、播放、音量控制、网络音频协议（NEP2 用途待确认） |
| 视频 | `media_kit_video/native_event_loop/libs`、`libmpv-2.dll` | 基于 libmpv 的视频播放 |
| RTC | `ZegoExpressEngine.dll`、`zego_express_engine_plugin`、`zego_data_plugin` | 实时语音（进房/推流/连麦） |
| 屏幕 | `screen_capturer_windows_plugin`、`screen_retriever`、`skyengine_iop_plugin` | 屏幕采集/录制、游戏内覆盖（skyengine SDK） |
| WebView | `WebView2Loader.dll`、`flutter_inappwebview_windows_plugin`（私有 fork `release/v6.2.0-beta.3.8`） | 应用内浏览器 |
| 存储 | `sqlite3.dll`、`stellaris_mmkv_windows_plugin`、`flutter_secure_storage_windows_plugin` | SQLite / MMKV / DPAPI 安全存储 |
| 加解密 | `fast_rsa_plugin.dll`、`librsa_bridge.dll`、`key.bin`、`dll_manifest.json/.sig` | RSA 运算与 DLL 完整性校验 |
| 崩溃 | `aa_tt_crash_pad_handler_plugin`、`report_crash_wer.dll` | Crashpad + WER 上报 |
| 系统 | `dartjni.dll`（Dart↔JNI 桥）、`process_threads_api`、`win32_system_info_api`、`system_clock`、`pasteboard`、`desktop_drop/multi_window`、`window_manager`、`permission_handler`、`url_launcher`、`share_plus` 等 | 平台能力 |

### 4.4 网络加速 SDK（`accel_sdk/sdk_x64`，OpenICP 风格开源方案定制）

| 层 | 组件 | 说明 |
|---|---|---|
| 用户态核心 | `OpenCore.dll`、`OpenKernel.dll`、`OpenNetfilter.dll`、`HGate.dll`、`Debug.dll` | 加速调度内核 |
| 代理出口 | `HttpProxy.exe`、`ConsoleProxy.exe`、`WintunSocks.exe`/`WintunSocksDll.dll` | HTTP/SOCKS 代理与 Wintun 出口 |
| 抓包/过滤 | `openconsolewpcap.dll`（WinPcap）、`openconsolepacket.dll`、`openconsolenpfx32/64.sys` | 旁路抓包（含 Win7 兼容驱动） |
| WFP 驱动 | `nfwfp64.sys` / `nfwfp86.sys` / `nfwfpArm64.sys` | WFP 标注回调，重定向游戏流量 |
| 主机捕获驱动 | `hostcap/{win7,win8,win10}/{amd64,i386,arm64}/ygate.sys`、`hostcap/x32|x64/Hgate.sys` | 按 OS 版本分发的 socket 拦截驱动 |
| 其他 | `nftdi64.sys`/`nftdi86.sys`、`wintun.dll` | TUN 虚拟网卡（WireGuard 系） |
| 证书 | `ca.cer`/`ca.crt` | 加速链路 HTTPS 处理用 CA（是否写入系统信任库需实测） |
| 许可 | `license_pcap.txt`、`license_wintun.txt` | WinPcap/Wintun 许可证（合规注意点） |
| 配置 | `config/{setting,game,ipmask,node_cache}.json`、`client.txt`、`verinfo.json` | **全部为加密存储**（Base64 封装密文，非明文） |

---

## 5. 数据存储与本地资产

| 位置 | 内容 | 备注 |
|---|---|---|
| `%APPDATA%\TTVoice\` | 安装本体 + `dll_manifest.json`/`.sig`/`key.bin`（17 B） | 清单覆盖 76 个文件（根目录 DLL + accel_sdk 子路径），**不含 .exe、不含 data/、不含 `%LOCALAPPDATA%` 两棵独立安装树** |
| `%LOCALAPPDATA%\tt_media_player\` | 媒体播放器完整安装树（含 `data/app.so`） | 独立 Flutter 应用 |
| `%LOCALAPPDATA%\tt_sound_enhancer\` | FXSound 引擎、`fxvad.sys` 驱动、`.fac` 游戏音效预设（apex/cs2/deltaforce/pubg/valorant/general）、`TTOptimizer\mmkv\global\tt.global.tt372279026` | MMKV 实例含设备号 `tt372279026` |
| `%LOCALAPPDATA%\tt-updater\` | 更新器数据 | 空（未更新时） |
| `%LOCALAPPDATA%\flutter_webview_windows\`、`TTVoice.exe.WebView2\EBWebView` | WebView2 用户数据（Cookie/缓存/令牌持久化） | EBWebView 为自定义 profile 名 |
| `Documents\TTVoice\analytics\instance_0_MyDatabase.db` | 埋点分析 SQLite（WAL 模式） | |
| `Documents\TTVoice\apm\*_se_pb_data_tt_tt.db` | APM 性能数据（Protobuf 序列化） | |
| `Documents\TTVoice\ttlog\process_0\`、`media_player\` | xlog 加密日志（`log_20261022.xlog` + `log.mmap3`）、`memory_info.txt`、`zegoav_logs`（ZEGO 引擎日志，明文 txt） | ZEGO 日志为明文 |
| `Documents\TTVoice\reportservice\reportService_20261022.log` | 崩溃上报服务日志 | |

**存储栈**：MMKV（`stellaris_mmkv`）+ SQLite FFI + flutter_secure_storage（Windows DPAPI）+ xlog（Mars 加密日志）。凭据/令牌类数据的落盘加密强度需动态验证（MMKV 默认是否加密、secure_storage 作用域）。

---

## 6. 安全机制与攻击面枚举

### 6.1 已有安全控制（实测证据）

| 控制 | 实现 | 评估 |
|---|---|---|
| DLL 完整性 | `dll_manifest.json`（SHA-256, schema v2）+ `dll_manifest.sig`（64 B，Ed25519 签名长度特征）+ `key.bin` | 覆盖 76 个 DLL；**未覆盖 .exe、Flutter 资产、两棵 LOCAL 安装树**；`key.bin` 仅 17 B（疑为密钥标识/OBUSCATE 引用，验证逻辑需逆向确认） |
| 配置加密 | `setting.json`/`game.json`/`client.txt`/`verinfo.json`/`cp.config`/`upgrade_context.json` 均为密文 | 防静态篡改；密钥必然内嵌于 `OpenCore.dll` 等二进制，属可提取设计 |
| 日志加密 | xlog 格式（Mars） | 引擎日志（zegoav）为明文，需关注令牌泄露 |
| 崩溃隔离 | Crashpad ×7 + `report_crash_wer.dll` + 独立 `report_service.exe` | dump 文件可能含敏感内存，需检查落盘 ACL |
| WebView 令牌注入 | `request_hook.js` hook `fetch`/`XHR`，注入 `x-tt-web-authorization`；含 prelogin 可信源白名单与 fail-closed 设计 | 设计较严谨；白名单可控性与同源策略边界需实测 |
| JS Bridge | `js_bridge.js`/`js_bridge_async.js`（`prompt()` 通道，`TTJSBridge.invoke(module, func, params, cb)`） | **无源码级白名单证据；页面可通过 prompt 调用原生能力，属 RCE 高发设计** |

### 6.2 攻击面清单（按信任域）

| # | 攻击面 | 入口/输入 | 关键测试点 |
|---|---|---|---|
| A1 | DLL 劫持/置换 | 安装目录（用户可写）中 45+ DLL、两棵 LOCAL 安装树 DLL | 篡改未列入 manifest 的 DLL；清单校验的触发时机（启动时/加载时）与失败行为（是否拒绝运行） |
| A2 | 更新链路 | `upgrade.exe`、`upgrade_context.json`、`tt-updater` | 更新包签名校验、降级/回滚保护、下载通道 TLS、断点续传中间件注入 |
| A3 | Windows 服务 | `TTAcceleratorService`（服务账户、镜像路径、可写路径）、驱动按需加载 API | 服务账户权限、参数注入、未授权驱动加载 |
| A4 | 内核驱动 | `nfwfp*.sys`、`ygate/Hgate.sys`、`nftdi*.sys`、`fxvad.sys`、`wintun.dll` | IOCTL 模糊测试、任意内存读写原语、驱动签名有效性、LOLDrivers 比对 |
| A5 | 加速器流量面 | WFP 重定向、HTTP/SOCKS 代理端口、Wintun 网卡、`ca.crt` | 是否写入系统根证书库；明文回退；代理认证；DNS 劫持处理 |
| A6 | WebView/JS Bridge | 任意加载页面（含远程 H5、url_launcher 外跳）、`prompt()` 桥 | 函数白名单、origin 校验、参数注入、`postMessage`、远程调试端口、file:// 访问 |
| A7 | RTC 协议 | ZEGO UDP 媒体面 + 私有 token | 伪造/重放 token、畸形包、未授权进房 |
| A8 | 业务 API | dio HTTP + gRPC | TLS 校验与 pinning、请求签名机制（`x-tt-web-authorization`）、错误响应健壮性 |
| A9 | 多进程 IPC | `desktop_multi_window`/`tt_multi_process`（通道类型待确认）、主进程↔媒体播放器↔音效器 | IPC 命名空间与 ACL、跨会话/跨用户消息注入、命令行参数注入 |
| A10 | 本地数据 | MMKV/SQLite/secure_storage/xlog/Crashpad dump/WebView profile | 落盘 ACL、注销后残留、DB 明文敏感字段、dump 内存敏感数据 |
| A11 | 输入面 | 文本/文件拖放（desktop_drop）、图片解码（extended_image/crop_image）、剪贴板、URL scheme、文件选择 | 畸形图片、超长路径、Unicode/ADS、循环 URL scheme |
| A12 | 供应链 | 私有 pub 源 `http://ttlite.ttyuyin.com:4000`（**明文 HTTP**）、私有 GitLab fork（inappwebview、desktop_multi_window）、ZEGO 私有版 | 开发/构建链风险；fork 与上游安全补丁的漂移；运行时下发（EasyTier 组件未见本地二进制，疑运行时下载） |

### 6.3 初步风险观察（供测试优先级排序）

1. **JS Bridge 为最高优先级动态测试对象**（`prompt()` 模式 + 远程 H5 混合内容）。
2. **内核驱动群**（尤其自研 `nfwfp`/`ygate`/`Hgate`）为最高优先级审计对象；第三方 `fxvad.sys` 需做版本与签名核查。
3. **完整性清单覆盖缺口**（.exe、LOCAL 树、资产）建议在自研产品设计中避免。
4. **供应链明文 HTTP pub 源**、私有 SDK fork 需纳入供应商风险管理。
5. `ca.crt` 是否落系统信任库直接决定加速模式下的 TLS 安全边界，必须实测。

---

## 7. 健壮性测试计划（测试矩阵）

> 原则：先静态基线 → 再故障注入 → 再协议/输入模糊化 → 最后 soak。每个用例记录：前置条件、步骤、期望行为（优雅降级/明确报错/自动恢复）、实际行为、缺陷定级。

### 7.1 静态基线与环境核查（T1）
- [ ] 全文件 SHA-256 清单与 `dll_manifest.json` 比对（验证覆盖缺口）
- [ ] 全部 .sys/.exe/.dll 签名链验证（`sigcheck -i -h`，是否 WHQL/EV、时间戳）
- [ ] LOLDrivers 数据库比对（已知漏洞驱动）
- [ ] 服务账户与权限：`sc qc TTAcceleratorService`、`accesschk`
- [ ] 数据目录 ACL：`icacls`（Documents\TTVoice、EBWebView、tt_media_player）
- [ ] 静态提取字符串：端点域名、URL scheme、端口（`strings`/`floss`）

### 7.2 安装/更新/卸载健壮性（T2）
- [ ] 静默安装/修复/卸载残留；重复安装；低权限用户安装
- [ ] 升级/降级/跨版本迁移；更新中断电/断网/杀进程后的恢复与回滚
- [ ] 篡改 `upgrade_context.json` 后触发的行为（是否拒绝/崩溃/静默失败）

### 7.3 启动与进程模型健壮性（T3）
- [ ] 冷启动/热启动；加速服务未启动、驱动加载失败、音效器缺失时的降级路径
- [ ] 并发双开、多显示器/DPI 变化/分辨率热切换、睡眠唤醒、注销前未退房
- [ ] 杀helper 进程矩阵：依次杀掉 `tt_media_player`、`tt_sound_enhancer`、`TTAcceleratorService`、`crashpad_handler`，观察主进程行为

### 7.4 输入模糊化（T4）
- [ ] 所有文本输入：超长（>64K）、零宽/组合 Unicode、RTL、控制字符
- [ ] 文件面：畸形 PNG/JPEG/GIF/WebP、超大图、零字节、伪装扩展名、ADS、超长路径
- [ ] URL scheme：自定义协议注册项枚举（注册表 `HKCR\<scheme>`），畸形/循环/超长 URL
- [ ] 剪贴板：超大内容、富文本、恶意格式

### 7.5 网络与协议健壮性（T5）
- [ ] Burp/Fiddler 代理 HTTP + gRPC-Web：篡改状态码/响应体（截断、超时、超大 JSON、循环引用）
- [ ] TLS：自签名证书、过期证书、域名不匹配、固定证书后观察 pinning 行为
- [ ] 网络工况：断网、弱网（clumsy 丢包/抖动/限速）、切网（Wi-Fi↔有线↔移动热点）、DNS 污染、hosts 劫持
- [ ] RTC：进房中拔网线/切网、50%+ 丢包、麦克风/扬声器热插拔、耳机电量耗尽断连
- [ ] 加速器：开启加速后与其他 VPN/WFP 安全软件共存、游戏进程不存在、加速节点超时

### 7.6 WebView 与 JS Bridge 专项（T6，安全向）
- [ ] 枚举 `TTJSBridge` 可调用 module/func（页面内探测 + 原生侧 hook 验证）
- [ ] 从恶意/第三方页面（file://、data:、远程域）调用桥函数，验证 origin 白名单
- [ ] 令牌泄露：构造跨域 fetch/XHR，观察 `x-tt-web-authorization` 是否按可信源清单注入
- [ ] WebView 配置：远程调试端口、`--allow-file-access-from-files`、混合内容、弹窗/外跳管控

### 7.7 资源与稳定性 Soak（T7）
- [ ] 72h soak：主进程 RSS/句柄/GDI/线程趋势、MMKV 与 SQLite WAL 增长、日志轮转策略
- [ ] 长时语音房（24h）内存与音频延迟趋势；循环进出房 500 次
- [ ] 磁盘满（数据目录所在卷 <100 MB）、只读数据目录、配额限制

### 7.8 故障注入与异常场景（T8）
- [ ] 运行中删除/改名数据文件（MMKV、DB、日志目录）后操作
- [ ] 系统时间跳变（NTP 校正、跨时区）对 token/证书/日志的影响
- [ ] 快速连续触发：进房/退房、开关麦、切设备、开关加速、窗口拖拽到屏幕外
- [ ] 崩溃恢复：Crashpad dump 生成后的重启行为；`report_service` 异常时主进程降级

### 7.9 驱动与服务安全测试（T9，需授权环境）
- [ ] IOCTL 模糊测试（Driver Verifier + 自研 fuzzer；先快照再执行）
- [ ] 驱动加载时序：无签名强制（测试模式）行为、按需加载失败回退
- [ ] 加速服务 RPC/管道面枚举（`pipeList`、`nmap` localhost）

### 7.10 数据安全与隐私核查（T10）
- [ ] MMKV/SQLite 明文敏感字段审计（token、手机号、设备号、联系人）
- [ ] 注销/删号后残留扫描；Crashpad dump 敏感内存扫描
- [ ] EBWebView profile Cookie/令牌留存；secure_storage 作用域（当前用户 vs 机器）
- [ ] 第三方外发：Firebase/APM/埋点端点的数据内容与同意流程

### 7.11 推荐工具栈
Procmon/Process Explorer/Process Monitor、API Monitor、WinDbg + Application Verifier、Driver Verifier、Radar（句柄/GDI）、clumsy（弱网）、Burp Suite（协议）、Wireshark + zegoav 日志对照、sigcheck/accesschk/pipeserverlist（Sysinternals）、Python + scapy（RTC 畸形包）。

---

## 8. 对自研产品的架构启示（d:/AIProjects/TTVoice）

| 维度 | TTVoice 做法 | 自研建议（Python 技术栈） |
|---|---|---|
| 进程隔离 | 媒体播放/音效/加速/崩溃处理全部独立进程 + 服务 | 沿用：重负载组件（媒体、网络代理）独立进程；服务仅保留必要面，最小权限账户 |
| 内核驱动 | 9+ 驱动（WFP/抓包/VAD） | **尽量避免内核态**；加速能力优先用户态（WFP 需驱动的场景评估供应商 SDK 的签名与 CVE 记录） |
| 完整性保护 | DLL 清单+签名（覆盖有缺口） | 覆盖全部可执行体（exe/dll/资源/配置），含子安装树；校验失败必须 fail-closed |
| 配置与密钥 | 配置加密但密钥内嵌 | 密钥不入二进制；用 DPAPI/CNG 或服务端下发短期凭据 |
| WebView 混合内容 | prompt() JS 桥 + 请求钩子注入令牌 | 桥函数显式白名单 + origin 绑定 + 按域最小授权；令牌仅注入可信源 |
| 更新 | 独立更新器 + 加密上下文 | 强制签名校验 + TLS + 防回滚 + 原子替换；断点续传可公开设计 |
| 日志 | xlog 加密 + 明文引擎日志 | 统一脱敏与加密；引擎/第三方日志纳入审计范围 |
| 存储 | MMKV/SQLite/DPAPI 混合 | 敏感字段列级加密；DB 与日志目录 ACL 收紧；提供一键清理 |
| 供应链 | 私有 HTTP pub 源、私有 SDK fork | 全部 HTTPS + 签名/哈希锁定；fork 纳入补丁跟踪；运行时下载组件校验哈希 |

---

## 9. 结论与后续步骤

1. 本报告完成 **架构画像与攻击面基线**：TTVoice 的复杂度集中在「多进程 × 多 Native SDK × 内核驱动 × WebView 混合内容」四者的交叉面。
2. 建议后续按 **T6（JS Bridge）→ T9（驱动/服务）→ T1（静态基线）→ T5（协议）→ T7（soak）** 的顺序展开具体测试，每轮输出缺陷单并回流到第 6.3 节风险清单。
3. 需要动态验证的关键假设：`key.bin`/签名算法、manifest 校验时机与覆盖、加速 CA 是否入系统信任库、JS Bridge 白名单、MMKV 加密状态、EasyTier 组件分发方式。

---

## 附录 A：安装目录核心文件清单（节选）

```
TTVoice.exe                    11,306,400 B  主程序（Flutter 容器）
data/app.so                    36,488,096 B  Dart AOT 业务代码
data/icudtl.dat / flutter_assets/           ICU 数据 / Flutter 资产
dll_manifest.json / .sig / key.bin          DLL 完整性校验三件套
upgrade.exe / uninstall.exe / upgrade_context.json（加密）   更新卸载
TTVoiceDelayedStart.exe                     延迟启动
tt_accelerator_service.exe                 加速服务（TTAcceleratorService）
TTOptimizer.exe / tt_netfixer.exe / tt_sound_enhancer.exe   系统优化/网络修复/音效
media_player.exe                           独立媒体播放器
report_crash_handler.exe / report_service.exe / report_crash_wer.dll   崩溃体系
ZegoExpressEngine.dll / zego_*_plugin.dll  RTC SDK
libmpv-2.dll / media_kit_*                 视频播放
TTAudioEffect.dll / NEP2.dll / librsa_bridge.dll / fast_rsa_plugin.dll  音频/网络/密码
skyengine_iop_plugin_plugin.dll           游戏内覆盖
WebView2Loader.dll / flutter_inappwebview_windows_plugin.dll   WebView2
sqlite3.dll / stellaris_mmkv_windows_plugin.dll / flutter_secure_storage_windows_plugin.dll  存储
accel_sdk/sdk_x64/**                       网络加速 SDK（驱动/代理/加密配置/CA）
```

## 附录 B：第三方与私有依赖清单

| 依赖 | 版本/来源 | 风险备注 |
|---|---|---|
| zego_express_engine | 3.25.0-private.559（pub.dev override） | 私有定制版，需跟踪上游 CVE |
| flutter_inappwebview | GitLab fork `release/v6.2.0-beta.3.8` | 私有 fork，补丁漂移风险 |
| desktop_multi_window | GitLab fork | 同上 |
| flutter_local_notifications_windows | GitLab fork ref 1.0.4 | 同上 |
| stellaris_* / apm_flutter_sdk / skyengine_* / tcloud_auth | 自建 pub 源 `http://ttlite.ttyuyin.com:4000` | **明文 HTTP**；来源可信性依赖内网安全 |
| WinPcap / Wintun | 捆绑 `license_pcap.txt` / `license_wintun.txt` | WinPcap 已停止维护，确认捆绑版本 |
| FXSound 引擎 + fxvad.sys | `%LOCALAPPDATA%\tt_sound_enhancer\engine` | 第三方音频驱动，版本与签名需核查 |
| Crashpad | `aa_tt_crash_pad_handler` | dump 内容敏感 |
| Firebase（stellaris_firebase） | 插件 | 数据出境/合规评估 |

## 附录 C：关键证据路径索引

- 版本与产品信息：`TTVoice.exe` VersionInfo；`data/flutter_assets/pubspec.yaml`
- 依赖与模块：`pubspec.yaml`（workspace/dependencies/dependency_overrides）
- 完整性：`dll_manifest.json`、`dll_manifest.sig`、`key.bin`
- 加速 SDK：`accel_sdk/sdk_x64/**`（verinfo.json、config/、driver/、hostcap/、console/、license/）
- JS 桥与请求钩子：`data/flutter_assets/packages/tt_js_bridge/assets/js_bridge{,_async}.js`、`packages/tt_webview/lib/src/interceptor/request_hook.js`
- 本地数据：`Documents\TTVoice\{analytics,apm,ttlog,reportservice}`、`%LOCALAPPDATA%\tt_sound_enhancer\TTOptimizer\mmkv\`
- 运行时：进程/服务/驱动实测记录（见 4.1）

---

*报告完。本文档为黑盒静态+运行时勘查结论；所有标注「待确认/推测」的项应在动态测试阶段验证后更新。*
