# CDP 选择器点击（WebView2 直连）

> 版本：script 插件 0.4.0 / version.dll 代理配套改动。
> 解决的问题：H5 弹层里「会动的数据」点不准——坐标点击（SendInput）只能点死位置，
> CDP 按 DOM 元素点击，元素在哪就点哪。

## 链路

```
TTVoice.exe 启动
  └─ version.dll (静态导入, DllMain)
       └─ EnableWebView2Cdp(): 给 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
          追加 --remote-debugging-port=9223   ← 必须早于第一个 WebView2 环境
             ↓ WebView2 浏览器进程起来后，127.0.0.1:9223 开出 DevTools 端点
script 插件（script_engine.inc 的 CDP 节，WinHTTP，无第三方依赖）
  └─ 事件带 selector 时：
       GET /json/list                    → 挑 type=="page" 的目标页
       WebSocket 升级 /devtools/page/<id>
       Runtime.evaluate                  → querySelector + scrollIntoView(nearest)
                                           + getBoundingClientRect（CSS px 视口坐标）
       Input.dispatchMouseEvent          → mouseMoved → mousePressed/Released
                                           （双击第二对 clickCount=2，右键 button=right）
```

要点：

- **可信输入**：CDP 的 Input 域走 Chromium 真实输入管线，页面拿到的事件
  `isTrusted=true`，与 OS 鼠标消息同路——这就是「把鼠标点击还原成消息事件」。
  与 `el.click()` 的合成事件（`isTrusted=false`，H5 页面可不认）本质不同。
- **不抢前台**：不 SetForegroundWindow、不做遮挡检查；窗口在后台也能点。
- **失败回退**：端点没开 / 元素找不到 / 派发失败 → 回退本行坐标的 SendInput
  点击（x/y 都是 0 视为「没拾取过」，跳过并打日志）。纯坐标行为不变。

## 脚本 JSON 新增字段

```json
{
  "version": 1,
  "name": "活动",
  "target_url": "act",            // 可选：多页面时按 URL 子串挑页（大小写不敏感）；
                                  // 空 = 唯一页直接用，多页取列表最后一个（新页在后）
  "events": [
    {"x": 27, "y": 113, "ox": 0, "oy": 0, "click": "left", "delay_ms": 600,
     "wheel": 0, "selector": "#btn-get", "remark": ""}
  ]
}
```

- `selector`（可选，CSS）：填了本行先走 CDP；`x/y/ox/oy` 是它的后备坐标。
- `wheel` + `selector`：滚轮落在元素中心，CDP `deltaY = -100 * 格数`（正=向上，
  与 Windows 滚轮语义一致）。
- 老脚本不带这两个键 → 行为与 0.3 完全一致。

## 日志字样（DLL 与编辑器试跑逐字对齐）

```
script: cdp connect ok target="https://…/act"
script: cdp click kind=0 "#btn-get" 640,352 (page px, trusted input)
script: cdp wheel +2 "#list" 640,300 (page px)
script: cdp "#btn-get" not found and no x/y fallback, skipped
script: cdp "#btn-get" missing, click falls back to x/y
script: cdp unavailable (no page target on 127.0.0.1:9223) - selector rows fall back to x/y
```

## 改动文件

| 文件 | 内容 |
|---|---|
| `proxy/dllmain.cpp` | `EnableWebView2Cdp()`：DllMain 里追加调试端口参数（kernel32-only，无 loader-lock 风险）；PayloadThread 打一行 `webview2 cdp args: …` |
| `plugin/script-engine/script_engine.inc` | CDP 节（连接/选页/evaluate/dispatch）+ `RunEventActions`（wheel→paste→click 的 CDP/坐标统一分派）+ selector/target_url 解析 |
| `plugin/script-engine/script_plugin.cpp` | ActEvent 改调 `RunEventActions`；RunNow 里 `CdpRunBegin`（每轮一次重连机会）；shutdown 时 `CdpShutdown`；版本 0.4.0 |
| `script_editor.py` | `CdpClient`（纯标准库 WS 客户端）+ 事件对话框 selector 字段/「测试选择器」按钮 + 脚本级 目标页URL 输入 + 试跑镜像 |
| `plugin/script-engine/test_cdp.cpp` | C++ 侧验证工具（见下） |

## 怎么验证（本机即可复跑）

```bat
:: 1. 起一个带端点的真实 Chromium（Edge 与 WebView2 同内核）
msedge --headless=new --remote-debugging-port=9223 ^
       --user-data-dir=%TEMP%\cdp-test-profile ^
       file:///D:/PyProjects/PlusPlugin/.cdp_test/cdp_test.html

:: 2. C++ 侧（引擎本体）：VS x64 环境里
cd plugin\script-engine && cl /nologo /MT /W4 /utf-8 test_cdp.cpp /link winhttp.lib user32.lib
test_cdp.exe            :: 期望最后一行 MAIN: CDP_TEST_OK
```

测试页（`.cdp_test/cdp_test.html`）记录每个事件的 `isTrusted`、坐标，所以
通过 = 点击真的进了可信输入管线，不只是调用没报错。python 侧
（`script_editor.CdpClient`）同一页可用交互方式验证，两侧已双双通过
（2026-10-25，Edge 156 / SDK 10.0.26100）。

## 踩坑记录（改一边必须想另一边）

- `WinHttpSetOption(WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET)` 传 `(NULL, 0)`，
  传 `&BOOL, 4` 直接 `ERROR_INVALID_PARAMETER(87)`。
- `WinHttpWebSocketReceive` 返回 **DWORD 错误码**（成功=0），不是 BOOL。
- 请求路径必须带前导 `/`（`/devtools/page/<id>`），否则升级选项同样 87。
- 选择器以 JSON 字面量直接嵌进 JS 表达式（JSON 字符串字面量 ⊂ JS 字面量），
  不要 `JSON.parse(字面量)`——那是把去引号后的内容再当 JSON 解析，必炸。
- 端口 9223 在三处必须一致：`proxy/dllmain.cpp kCdpArg`、
  `script_engine.inc kCdpPort`、`script_editor.py CDP_PORT`。

## 边界与已知限制

- 每轮运行对「连不上」只尝试一次（`CdpRunBegin` 复位）；运行中断线则下一
  个 selector 行自动重连重选目标页。
- 选择器只查主 frame 的 `document.querySelector`，**不支持 iframe 内元素**；
  H5 弹层是同页 DOM 覆盖层，通常够用，需要时再扩展。
- 调试端口对本机所有进程可见，等于给页面开了本地控制通道——这是自动化
  工具的预期行为，介意的话改端口常量三处或加 `--remote-debugging-pipe`。
