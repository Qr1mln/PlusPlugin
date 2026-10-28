# framework 协议规格（version.dll ↔ framework.dll）

> 状态：**草案 v0**（分支 `proxy2.0`）。
> 目标：在**保留热加载**的前提下，把 `version.dll` 与 `framework.dll` 的边界，从
> 「结构体 + 函数指针表 + P_ABI 版本号」降级为 **稳定的文本命令协议**，使 ABI 版本号
> 失去存在理由。代号：**单入口 + JSON 协议**。

## 1. 设计目标与不变量

- **唯一不变式**：两个函数签名（§2）**永不改变**。所有演进都发生在 JSON 内部。
- **向后兼容**：新增能力 = 新增命令名，字段**只增不删**；未知命令只回错误，不崩、不拒绝加载。
- **热加载保留**：仍走 `LoadLibrary` → `GetProcAddress("FrameworkEntry")` → … → `FreeLibrary`。
- **语言中立**：host = C / 汇编（version.dll），guest = C# NativeAOT（framework.dll）。
- **零外部依赖新增**：控制流走文本；大数据块走 buffer 旁路（§7）。

## 2. 边界：两个固定契约

```c
// guest 唯一导出符号，签名永不改变。
// 返回值 >= 0：已写入 resp 的字节数（不含结尾 NUL）
//       < 0：所需字节数（取绝对值），调用方扩容后重试
// respJson 由「调用方」提供；被调方只写入，不负责分配/释放。
int FrameworkEntry(const char* reqJson, char* respJson, int cap);

// host 传给 guest 的唯一函数指针，签名同样永不改变。
typedef int (*HostDispatchFn)(const char* reqJson, char* respJson, int cap, void* user);
```

**为什么"调用方分配 buffer"**：C++ 用 CRT 堆、C# 用 native allocator，跨堆 `free` 是未定义行为。
由调用方分配、被调方只写，彻底绕开「谁分配谁释放」这条隐性 ABI。

**方向**

```
version.dll  ──FrameworkEntry(req, resp, cap)──▶  framework.dll   (host → guest)
version.dll  ◀──HostDispatch(req, resp, cap)────  framework.dll   (guest → host)
```

## 3. 返回值与缓冲区语义

| 返回值 | 含义 |
|---|---|
| `>= 0` | 已写入 `resp` 的字节数（不含 NUL），内容已 NUL 结尾 |
| `< 0` | 需要 `-ret` 字节；调用方扩容后重试 |
| `0` | 合法空响应（精确定义见 §11） |

- 首次调用建议 `cap = 4096`；单次文本命令默认上限 **64 KiB**，超出走 §7 旁路。
- `req` / `resp` 均为 **UTF-8 + NUL 结尾**。

## 4. 生命周期时序

```mermaid
%%{init: {'look':'handDrawn','theme':'base','themeVariables':{'background':'transparent','lineColor':'#ffffff','textColor':'#ffffff','primaryTextColor':'#ffffff','signalColor':'#ffffff','signalTextColor':'#ffffff','actorTextColor':'#ffffff','actorLineColor':'#ffffff','actorBkg':'transparent','actorBorder':'#ffffff','labelBoxBkgColor':'transparent','labelBoxBorderColor':'#ffffff','labelTextColor':'#ffffff','noteBkgColor':'transparent','noteBorderColor':'#ffffff','noteTextColor':'#ffffff'}}}%%
sequenceDiagram
    participant V as version.dll (host)
    participant F as framework.dll (guest)

    V->>F: LoadLibrary + GetProcAddress("FrameworkEntry")
    V->>F: FrameworkEntry({cmd:"init", dispatch, user, ini})
    F-->>V: {ok:true, proto:1}
    Note over F: 启动 worker / WebSocket 线程
    V->>F: FrameworkEntry({cmd:"geometry"})
    F-->>V: {ok:true, ...}
    F->>V: HostDispatch({cmd:"publishLog", text})
    V-->>F: {ok:true}
    V->>F: FrameworkEntry({cmd:"shutdown"})
    F-->>V: {ok:true}
    V->>F: FreeLibrary
```

## 5. 消息封套

**请求**：`{"cmd":"<命令名>", ...参数}`

**响应**：统一两种形态

```json
{"ok":true,  ...结果字段}
{"ok":false, "err":"unknown command", "code":404}
```

## 6. 命令清单

### 6.1 host → guest（经 `FrameworkEntry`）

| cmd | 参数 | 响应 | 说明 |
|---|---|---|---|
| `init` | `dispatch`(十进制指针串), `user`(十进制指针串), `ini`(对象, 可选) | `{"ok":true,"proto":1}` | 建立边界；guest 记下 `dispatch`/`user` 并启动线程 |
| `shutdown` | — | `{"ok":true}` | 正常卸载前调用；guest 停止线程 |
| `logLine` | `text` | `{"ok":true}` | host 主动把日志推给 guest（可选） |

`init` 示例（指针用**十进制字符串**传，避免 JSON number 精度问题）：

```json
{"cmd":"init","dispatch":"140712345678903","user":"140712345678904","ini":{"target":"TTVoice.exe","debug":false}}
```

### 6.2 guest → host（经 `HostDispatch`）

| cmd | 参数 | 响应 | 对应旧 Host 能力 |
|---|---|---|---|
| `publishLog` | `text` | `{"ok":true}` | `publishLog` |
| `mainWindow` | — | `{"ok":true,"hwnd":"12345"}` | `mainWindow` |
| `viewWindow` | — | `{"ok":true,"hwnd":"12345"}` | `viewWindow` |
| `scale` | — | `{"ok":true,"scale":1.25}` | `scale` |
| `onUiThread` | `cmd`(转发的子命令) | 同子命令响应 | 新增：需要宿主线程时使用 |
| `grabFrame` | `out`(十进制指针), `cap` | `{"ok":true,"w":..,"h":..,"format":..}` | 旁路，见 §7 |

> 旧 `submitTask` / `subscribeLog` 不再作为边界能力：framework 自持 worker 线程；
> 日志改由 guest 单向 `publishLog`，host 若要推给 guest 用 `logLine`。

## 7. 大二进制旁路

JSON 不适合传像素。截图等大数据走「buffer + 指针」约定，仍复用同一入口：

- `grabFrame`：guest 给出可写缓冲区指针与容量，host 把帧写进去，只回元数据：

```json
{"cmd":"grabFrame","out":"140712345678910","cap":33177600}
{"ok":true,"w":1920,"h":1080,"format":"bgra"}
```

- 两阶段兜底：`{"cmd":"grabFrame"}` → `{"ok":true,"need":<bytes>}` → guest 分配后再调一次。

**原则**：控制流走 JSON，大数据块走 buffer/指针；两者共用同一个 dispatch。

## 8. 热加载与安全

- 加载/卸载：`LoadLibrary` → `GetProcAddress("FrameworkEntry")` → `init` → 使用 → `shutdown` → `FreeLibrary`。
- **卸载前必须确认没有 guest 代码在栈上**（沿用在途计数 `inflight`；`shutdown` 返回后停止新调用）。
- 协议化的收益：host 不再持有「一张会悬空的函数指针表」，只持有一个 `init` 时交换的 `dispatch` 指针；
  重载后 guest 重新 `init` 拿新指针，天然重来。
- `DllMain` 内仍**不做重活**（不建线程、不分配）：引导工作放到 `PayloadThread`。

## 9. 版本与兼容

- **不做拒绝式版本校验**。`proto` 字段仅用于日志/诊断，**不用于拒绝加载**。
- 新增命令/字段 = 纯增量；双方对未知 `cmd` 回 `{"ok":false,"code":404}` 即可。
- 字段只增不删；缺字段用默认值。

## 10. 编码与线程

- **编码**：UTF-8，NUL 结尾。
- **线程**：`FrameworkEntry` / `HostDispatch` 可能被多线程调用，两端各自保证线程安全；
  跨边界的调用不得在持有 loader lock 时发起。

## 11. 错误码

| code | 含义 |
|---|---|
| `0` | 成功 |
| `400` | 请求不是合法 JSON |
| `404` | 未知命令 |
| `413` | 响应超出 `cap`（返回值为 `-ret`，调用方扩容重试） |
| `422` | 参数缺失/非法 |
| `500` | 内部错误 |

## 12. 待定项（Open）

- 命令最终命名（`init`/`shutdown` 还是 `hello`/`bye`）。
- `grabFrame` 用「指针」还是「共享内存 + 命名事件」。
- `proto` 字段是否需要（当前倾向仅诊断，不参与判定）。
- `onUiThread` 的子命令封装粒度。
- framework 内部 WebSocket 服务端实现路线（保留：C# `System.Net.WebSockets` / Fleck 等）。