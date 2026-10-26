"""pplugin 点击脚本编辑器（极简版）。

独立窗口，做三件事：
  1. 拾取坐标——点一下目标位置即记录（这次点击会被吞掉，不会触发应用），
     坐标按 FLUTTERVIEW 相对逻辑 px（96 dpi）保存；拾取期间目标窗口会盖上
     一层透明遮罩：十字线跟随鼠标、实时显示当前位置的逻辑/物理坐标，鼠标
     移出窗口时变红并提示「点了不算」；遮罩上右键 = 取消这次拾取；
  2. 编辑事件表（坐标 / 点击类型 / 间隔 / 滚轮 / 备注）。每一行的坐标就是
     该行自己的绝对落点（逻辑 px），改哪一行只影响哪一行；
  3. 存成 JSON 供 DLL 读取执行；本窗口也能直接「试跑」验证坐标。

脚本目录：%APPDATA%\\TTVoice\\scripts\\<名称>.json
保存时还会写同目录的 active.txt（记下刚保存的脚本名），DLL 优先跑这一个；
没有就跑最新的 .json。指定/运行某个脚本改由插件自带面板窗口完成。
运行：python script_editor.py

JSON 格式（DLL 侧读取的就是这一份）：

    {
      "version": 1,
      "name": "活动",
      "hotkey": "",
      "loop": 1,
      "events": [
        {"x": 27, "y": 113, "click": "left", "delay_ms": 600,
         "wheel": 0, "remark": ""},
        {"x": 1573, "y": 87, "click": "left", "delay_ms": 0,
         "wheel": 0, "remark": ""}
      ]
    }

字段含义：
  x, y          本行的执行坐标，每行独立；FLUTTERVIEW 客户区相对逻辑 px
                （96 dpi），物理 px = 逻辑 * DPI/96。改哪一行只影响哪一行
  ox, oy        已废弃：老脚本里的偏移在载入时折算进 x/y（x += ox），保存时
                删掉这两个键。DLL 仍兼容（缺省 0，即直接执行 x/y）
  click         left | double | right
  selector      可选 CSS 选择器。填了本行先走 CDP：Runtime.evaluate 定位元素
                （getBoundingClientRect，页面 CSS px），Input.dispatchMouseEvent
                在元素中心派发鼠标事件——事件走浏览器真实输入管线
                （isTrusted=true），不需要目标窗口在前台、不怕被遮挡。CDP
                不可用或页面里没有该元素时，回退到本行坐标的 SendInput 点击
                （x/y 都是 0 视为「没拾取过」，跳过并打日志）。勾了「后台消息」
                后，选择器行也往 view 投鼠标消息：拿选择器解析出的位置换算成
                view 坐标，PostMessage(WM_MOUSEMOVE/DOWN/UP)（不置前、不动光标，
                某些页面只认落在窗口上的消息，CDP 派发过去 DOM 会触发 click 但
                业务不响应）。换算只差一个 WebView 面板偏移，用「既有选择器又有
                坐标」的行标定一次即可。端口由
                version.dll 代理在宿主启动时布下（9223），编辑器试跑/测试
                选择器连的是同一个端口
  page          可选，本行要走的 WebView 页面（URL 子串，来自 /json/list）。
                编辑器试跑按它换页重连；空 = 用脚本默认页（最后一张「正经
                页」，跳过 file:// / deviceid 那类内部隐藏页）
  hotkey        F1..F24 / 单个字母或数字；空 = 不注册（避免抢应用自己的键）
  loop          整份脚本重复执行的轮数
  delay_ms      本行执行完之后的等待（下一行开始前）
  wheel         滚轮格数：正=向上，负=向下，0=不滚；有 selector 时滚在元素上
                （CDP deltaY = -100 * 格数，与 DLL 一致）
  remark        备注，随便写（原 note 键已更名）；DLL 在每个事件日志行里打印
                （script: event i/n: 备注），不参与执行，只给人/编辑器看
  view_size     编辑器额外写的 [w,h]（保存时的 view 客户区），DLL 不认这个键，只用来提醒 view 尺寸变了坐标可能偏

                注意：events 为空时 DLL 拒收（count>0 才算解析成功），会打
                script: parse failed <path> (no events / bad json)。
                门控 / 门控上限已整体移除：DLL 与编辑器都不再读
                wait_px / wait_max_ms，老脚本里的这两个键按未知键跳过。

与 DLL 的关系（很重要）：
  DLL（script_engine.inc）是最终交付的执行器；本编辑器是它的开发/调试镜像，
  所以「试跑」的每一步都与 DLL 逐字对齐：同一套消息序列、同一套坐标换算、
  同一套真实输入注入（H5 弹层不理会 PostMessage 的合成消息，只用 SendInput；
  点击点被其他窗口挡住时跳过并打日志）
  （int(逻辑px * DPI/96) 截断）、同一批日志字样（可与插件面板日志/%TEMP%
  日志逐行比对）。执行日志窗格可以勾「跟随 DLL 日志」把 DLL 自己写的行拉
  进来，两边放在一个窗里看。保存后还会打印「DLL 下次会加载哪份脚本」。
  只有一处故意不同：编辑器能「停止」（DLL 跑完为止）。
  其他任何行为差异都是 bug，要两边同步改。
"""
from __future__ import annotations

import base64
import ctypes
import http.client
import json
import os
import queue
import socket
import threading
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import messagebox, simpledialog, ttk

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# --- 常量 -----------------------------------------------------------------
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEWHEEL = 0x020A
WM_QUIT = 0x0012
MK_LBUTTON = 0x0001
MK_RBUTTON = 0x0002
WH_MOUSE_LL = 14
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020    # 鼠标事件穿透（点下去仍落在目标窗口上）
WS_EX_TOOLWINDOW = 0x00000080     # 不进任务栏 / 不进 Alt+Tab
WS_EX_NOACTIVATE = 0x08000000     # 不抢焦点
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010

MAIN_CLASS = "FLUTTER_RUNNER_WIN32_WINDOW"
VIEW_CLASS = "FLUTTERVIEW"

CLICK_LABELS = {"left": "左键单击", "double": "左键双击", "right": "右键单击"}
LABEL_CLICKS = {v: k for k, v in CLICK_LABELS.items()}

# click 名字 -> DLL 的 kind 数字（script_engine.inc ClickFromName）
CLICK_KINDS = {"left": 0, "double": 1, "right": 2}

# --- 与 DLL 对齐的常量/上限（改一边必须改另一边）-------------------------
DBLCLK_GAP_MS = 40        # 双击两条消息之间的 Sleep
MAX_EVENTS = 32           # kScriptMaxEvents：DLL 只读前 32 行
MAX_LOOP = 99             # loop 上限

# DLL 的日志文件（LogFromOverlay 写）
DLL_LOG = os.path.join(os.environ.get("TEMP", "tools"), "proxy.log")

# 与 overlay.inc IsWarnLine 同一批关键字：命中的日志行画琥珀色
WARN_MARKS = ("fail", "timeout", "busy", "ignored", "skipped", "WARN",
              "not seen", "no json")


# --- Win32 结构/签名 ------------------------------------------------------
class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("pt", wintypes.POINT),
        ("mouseData", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)

user32.EnumWindows.argtypes = (WNDENUMPROC, wintypes.LPARAM)
user32.EnumChildWindows.argtypes = (wintypes.HWND, WNDENUMPROC, wintypes.LPARAM)
user32.GetClassNameW.argtypes = (wintypes.HWND, wintypes.LPWSTR, ctypes.c_int)
user32.IsWindowVisible.argtypes = (wintypes.HWND,)
user32.GetClientRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
user32.ClientToScreen.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
user32.ScreenToClient.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
user32.PrintWindow.argtypes = (wintypes.HWND, wintypes.HDC, wintypes.UINT)
user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.GetDpiForWindow.argtypes = (wintypes.HWND,)
user32.SetWindowsHookExW.argtypes = (ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD)
user32.SetWindowsHookExW.restype = wintypes.HANDLE
user32.UnhookWindowsHookEx.argtypes = (wintypes.HANDLE,)
user32.CallNextHookEx.argtypes = (wintypes.HANDLE, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)
user32.CallNextHookEx.restype = ctypes.c_ssize_t
user32.GetMessageW.argtypes = (ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT)
user32.TranslateMessage.argtypes = (ctypes.POINTER(wintypes.MSG),)
user32.DispatchMessageW.argtypes = (ctypes.POINTER(wintypes.MSG),)
user32.PostThreadMessageW.argtypes = (wintypes.DWORD, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
user32.GetCursorPos.argtypes = (ctypes.POINTER(wintypes.POINT),)
user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
user32.GetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int)
user32.GetWindowLongPtrW.restype = ctypes.c_longlong
user32.SetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_longlong)
user32.SetWindowLongPtrW.restype = ctypes.c_longlong
user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wintypes.UINT)
user32.SetWindowPos.restype = wintypes.BOOL

kernel32.GetCurrentThreadId.restype = wintypes.DWORD
kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = (
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)


def enable_dpi_awareness() -> None:
    """必须在建 Tk 窗口前调用，否则拿到的是缩放后的假坐标。"""
    try:
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):  # PER_MONITOR_AWARE_V2
            return
    except Exception:
        pass
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
    except Exception:
        pass


def make_lparam(x: int, y: int) -> int:
    return (int(x) & 0xFFFF) | ((int(y) & 0xFFFF) << 16)


def get_class(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def proc_name(hwnd: int) -> str:
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(len(buf))
        ok = kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size))
        return os.path.basename(buf.value) if ok else ""
    finally:
        kernel32.CloseHandle(handle)


class Target:
    """当前找到的 pplugin 窗口（主窗口 + FLUTTERVIEW）。"""

    def __init__(self):
        self.main = 0
        self.view = 0
        self.w = 0
        self.h = 0
        self.scale = 1.0
        self.exe = ""

    @property
    def ok(self) -> bool:
        return bool(self.view and self.w > 0 and self.h > 0)

    def describe(self) -> str:
        if not self.ok:
            return "未找到 pplugin（先启动应用，再点「刷新」）"
        return "%s  view %dx%d  DPI %d%%" % (self.exe or "?", self.w, self.h, round(self.scale * 100))


def find_target() -> Target:
    t = Target()
    top = []

    def on_top(hwnd, _lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        if get_class(hwnd) == MAIN_CLASS:
            top.append(hwnd)
            return False
        return True

    cb_top = WNDENUMPROC(on_top)
    user32.EnumWindows(cb_top, 0)
    if not top:
        return t
    t.main = top[0]

    views = []

    def on_child(hwnd, _lp):
        if get_class(hwnd) == VIEW_CLASS:
            views.append(hwnd)
            return False
        return True

    cb_child = WNDENUMPROC(on_child)
    user32.EnumChildWindows(t.main, cb_child, 0)
    if not views:
        return t
    t.view = views[0]
    t.exe = proc_name(t.main)

    rect = wintypes.RECT()
    user32.GetClientRect(t.view, ctypes.byref(rect))
    t.w = rect.right - rect.left
    t.h = rect.bottom - rect.top

    dpi = 0
    getter = getattr(user32, "GetDpiForWindow", None)
    if getter is not None:
        getter.argtypes = (wintypes.HWND,)
        getter.restype = wintypes.UINT
        dpi = getter(t.view)
    t.scale = (dpi or 96) / 96.0
    return t


# --- 点击/滚轮 ------------------------------------------------------------
# 下面三个函数是 script_engine.inc SimulateClickKindAt / SimulateWheelAt /
# PasteTextToApp 的等价物：消息顺序、wParam、时间间隔都要一致。
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_WHEEL = 0x0800


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _INPUTU(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTU)]


user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT
user32.SetCursorPos.argtypes = (wintypes.INT, wintypes.INT)
user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
user32.WindowFromPoint.argtypes = (wintypes.POINT,)
user32.WindowFromPoint.restype = wintypes.HWND
user32.GetAncestor.argtypes = (wintypes.HWND, wintypes.UINT)
user32.GetAncestor.restype = wintypes.HWND


def send_mouse(flags: int, wheel: int = 0) -> None:
    inp = INPUT()
    inp.type = 0                       # INPUT_MOUSE
    inp.mi.dwFlags = flags
    inp.mi.mouseData = wheel & 0xFFFFFFFF
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def post_click(view: int, px: int, py: int, kind: str) -> tuple[bool, str]:
    """真实点击（与 DLL 相同）：TT语音的 H5 弹层不理会 PostMessage 的合成
    消息，只响应 OS 生成的输入。前置应用到前台 -> 遮挡检查 -> SetCursorPos +
    SendInput。返回 (是否发送, 信息)。"""
    main = user32.GetAncestor(view, 2)   # GA_ROOT
    pt = wintypes.POINT(px, py)
    user32.ClientToScreen(view, ctypes.byref(pt))
    user32.SetForegroundWindow(main)
    time.sleep(0.2)
    hit = user32.WindowFromPoint(pt)
    if hit != view:
        return False, get_class(hit) if hit else "?"
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.05)                     # 等悬停帧落下再按键
    down = MOUSEEVENTF_RIGHTDOWN if kind == "right" else MOUSEEVENTF_LEFTDOWN
    up = MOUSEEVENTF_RIGHTUP if kind == "right" else MOUSEEVENTF_LEFTUP
    send_mouse(down)
    send_mouse(up)
    if kind == "double":
        time.sleep(DBLCLK_GAP_MS / 1000.0)
        send_mouse(down)
        send_mouse(up)
    return True, "%d,%d" % (px, py)


def make_lparam(x: int, y: int) -> int:
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


def post_click_message(view: int, px: int, py: int, kind: str) -> tuple[bool, str]:
    """后台消息点击：往 view 直接投鼠标消息，不置前、不动光标、不看遮挡。

    H5 弹层不一定理会合成消息（所以坐标行默认还是走 SendInput 的真实输入），
    但 WebView 里的普通页面/原生控件通常认，而且能在后台跑。
    """
    if not user32.IsWindow(view):
        return False, "view gone"
    lp = make_lparam(px, py)
    user32.PostMessageW(view, WM_MOUSEMOVE, 0, lp)
    time.sleep(0.03)                      # 先移动，让 hover 状态落下
    if kind == "right":
        down, up, mk = WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON
    else:
        down, up, mk = WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON
    user32.PostMessageW(view, WM_MOUSEMOVE, mk, lp)
    user32.PostMessageW(view, down, mk, lp)
    user32.PostMessageW(view, up, 0, lp)
    if kind == "double":
        time.sleep(DBLCLK_GAP_MS / 1000.0)
        user32.PostMessageW(view, WM_LBUTTONDBLCLK, MK_LBUTTON, lp)
        user32.PostMessageW(view, WM_LBUTTONUP, 0, lp)
    return True, "%d,%d" % (px, py)


def post_wheel(view: int, px: int, py: int, notches: int) -> tuple[bool, str]:
    """真实滚轮：光标移到点上下滚。返回 (是否发送, 信息)。"""
    if not notches:
        return True, "%d,%d" % (px, py)
    main = user32.GetAncestor(view, 2)
    pt = wintypes.POINT(px, py)
    user32.ClientToScreen(view, ctypes.byref(pt))
    user32.SetForegroundWindow(main)
    time.sleep(0.2)
    hit = user32.WindowFromPoint(pt)
    if hit != view:
        return False, get_class(hit) if hit else "?"
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.05)
    send_mouse(MOUSEEVENTF_WHEEL, 120 * int(notches))
    return True, "%d,%d" % (px, py)


# --- CDP（选择器点击，与 DLL script_engine.inc 的 CDP 节逐字对齐）----------
# 端口由 version.dll 代理在进程启动时给 WebView2 布下（WEBVIEW2_ADDITIONAL_
# BROWSER_ARGUMENTS --remote-debugging-port）。编辑器自己连同一端口，试跑选择
# 器行时与 DLL 走同一套：Runtime.evaluate 定位元素 -> Input.dispatchMouseEvent
# 派发鼠标事件。CDP 输入走浏览器真实输入管线（isTrusted=true），不需要目标
# 窗口在前台、也不怕被遮挡。
CDP_HOST = "127.0.0.1"
CDP_PORT = 9223            # 与 proxy/dllmain.cpp kCdpArg、script_engine.inc kCdpPort 一致
CDP_DELTA_PER_NOTCH = 100  # DLL: deltaY = -100 * wheel（正格数=向上）

# 与 DLL CdpQuerySelector 的表达式一字不差（selector 以 JSON 字面量嵌入，
# JSON 字符串字面量本身就是合法的 JS 字符串字面量）
CDP_QUERY_JS = (
    "(function(){var e=document.querySelector(%s);"
    "if(!e)return '{\"found\":0}';"
    "e.scrollIntoView({block:'nearest',inline:'nearest'});"
    "var r=e.getBoundingClientRect();"
    "return JSON.stringify({found:1,x:r.left+r.width/2,y:r.top+r.height/2,"
    "w:r.width,h:r.height,vw:innerWidth,vh:innerHeight});})()"
)


# 导出页面结构：从 body 深度优先遍历，每个元素带上层级/标签/可用选择器/文本/
# 位置，用来在编辑器里挑选择器（与 CDP_QUERY_JS 同一条 Runtime.evaluate 通道）
CDP_DOM_JS = (
    "(function(){var out=[],MAX=400,"
    "NOISE={script:1,style:1,link:1,meta:1,noscript:1,template:1,head:1,title:1};"
    "function one(e){if(e.id)return '#'+e.id;"
    "var s=e.tagName.toLowerCase();"
    "var c=(typeof e.className==='string'?e.className.trim():'').split(/\\s+/)"
    ".filter(Boolean).slice(0,2);"
    "if(c.length)s+='.'+c.join('.');return s;}"
    "function walk(e,d){if(!e||out.length>=MAX)return;"
    "if(e.nodeType===1){var tg=e.tagName.toLowerCase();"
    "if(NOISE[tg])return;"                      # 噪声节点连子树一起跳过
    "if(d>0){var r=e.getBoundingClientRect();"
    "var t=(e.children.length?'':(e.innerText||e.textContent||''));"
    "t=String(t).replace(/\\s+/g,' ').trim().slice(0,40);"
    "out.push({d:d,tag:tg,id:e.id||'',"
    "cls:(typeof e.className==='string'?e.className.trim():''),sel:one(e),"
    "x:Math.round(r.left),y:Math.round(r.top),w:Math.round(r.width),"
    "h:Math.round(r.height),tx:t});}"
    "for(var i=0;i<e.children.length&&out.length<MAX;i++)walk(e.children[i],d+1);}}"
    "walk(document.body,0);"
    "return JSON.stringify({url:location.href,title:document.title,"
    "vw:innerWidth,vh:innerHeight,n:out.length,items:out});})()"
)


class CdpError(Exception):
    pass


class CdpClient:
    """最小 DevTools 客户端（纯标准库）：/json/list 选页 + 一条 WebSocket。"""

    def __init__(self, host: str = CDP_HOST, port: int = CDP_PORT):
        self.host = host
        self.port = port
        self._msg_id = 0
        self._sock: socket.socket | None = None

    # -- 目标页 --------------------------------------------------------------
    def list_pages(self) -> list[dict]:
        conn = http.client.HTTPConnection(self.host, self.port, timeout=3)
        try:
            conn.request("GET", "/json/list")
            resp = conn.getresponse()
            if resp.status != 200:
                raise CdpError("/json/list HTTP %d" % resp.status)
            data = json.loads(resp.read().decode("utf-8", "replace"))
        finally:
            conn.close()
        return [t for t in data
                if t.get("type") == "page" and t.get("webSocketDebuggerUrl")]

    @staticmethod
    def pick_page(pages: list[dict], match: str = "") -> dict:
        """DLL CdpPickTarget 同一套规则：match 子串优先，否则唯一页/最后一页。"""
        if match:
            low = match.lower()
            for page in pages:
                if low in (page.get("url") or "").lower():
                    return page
        if len(pages) == 1:
            return pages[0]
        if pages:
            return pages[-1]    # 新开的页追加在列表尾部
        raise CdpError("no page target on %s:%d" % (CDP_HOST, CDP_PORT))

    def connect(self, match: str = "") -> str:
        """连上目标页的 DevTools WebSocket，返回页面 URL（给日志用）。"""
        page = self.pick_page(self.list_pages(), match)
        ws = page["webSocketDebuggerUrl"]
        path = ws[ws.find("/devtools/"):]
        self.close()
        self._sock = socket.create_connection((self.host, self.port), timeout=5)
        self._sock.settimeout(5)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ) % (path, self.host, self.port, key)
        self._sock.sendall(request.encode())
        head = self._recv_until(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            self.close()
            raise CdpError("websocket upgrade failed")
        return page.get("url") or ""

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    # -- 帧收发（客户端发出的帧必须带掩码，服务端帧不带） ----------------------
    def _recv_exact(self, n: int) -> bytes:
        chunks = []
        while n > 0:
            assert self._sock is not None
            chunk = self._sock.recv(n)
            if not chunk:
                raise CdpError("socket closed")
            chunks.append(chunk)
            n -= len(chunk)
        return b"".join(chunks)

    def _recv_until(self, marker: bytes) -> bytes:
        data = b""
        while marker not in data:
            chunk = self._recv_exact(1)
            data += chunk
        return data

    def _send_frame(self, payload: bytes) -> None:
        assert self._sock is not None
        header = bytes([0x81])            # FIN + text
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + n.to_bytes(2, "big")
        else:
            header += bytes([0x80 | 127]) + n.to_bytes(8, "big")
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(header + mask + masked)

    def _recv_message(self) -> str:
        """收一条完整文本消息；分片拼接，ping 回 pong，close 报错。"""
        payload = b""
        while True:
            assert self._sock is not None
            h = self._recv_exact(2)
            fin, opcode, n = h[0] & 0x80, h[0] & 0x0F, h[1] & 0x7F
            if n == 126:
                n = int.from_bytes(self._recv_exact(2), "big")
            elif n == 127:
                n = int.from_bytes(self._recv_exact(8), "big")
            chunk = self._recv_exact(n)
            if opcode == 0x8:                        # close
                raise CdpError("websocket closed by peer")
            if opcode == 0x9:                        # ping -> pong
                pong = bytes([0x8A, 0x80]) + os.urandom(4)
                self._sock.sendall(pong)
                continue
            if opcode in (0x1, 0x0):                 # text / continuation
                payload += chunk
            if fin:
                return payload.decode("utf-8", "replace")

    def call(self, method: str, params: dict | None = None) -> dict:
        """一条命令一应答；事件（没有 id）丢弃。"""
        if self._sock is None:
            raise CdpError("not connected")
        self._msg_id += 1
        frame = {"id": self._msg_id, "method": method, "params": params or {}}
        self._send_frame(json.dumps(frame).encode())
        while True:
            resp = json.loads(self._recv_message())
            if resp.get("id") == self._msg_id:
                if "error" in resp:
                    raise CdpError(resp["error"].get("message", "cdp error"))
                return resp.get("result", {})

    def evaluate(self, expression: str) -> str:
        """Runtime.evaluate，返回页面的字符串结果（与 DLL CdpEvaluate 同构）。"""
        result = self.call("Runtime.evaluate", {"expression": expression})
        if result.get("exceptionDetails"):
            raise CdpError("page exception")
        inner = result.get("result", {})
        if inner.get("type") != "string":
            raise CdpError("evaluate returned %s" % inner.get("type"))
        return inner.get("value", "")


def cdp_query_selector(client: CdpClient, selector: str) -> dict:
    """与 DLL CdpQuerySelector 同一表达式、同一结果字典。"""
    return json.loads(client.evaluate(CDP_QUERY_JS % json.dumps(selector)))


# 应用自带的内部隐藏页（取 device id 用），DOM 没有可看的结构
NOISE_PAGE_MARKS = ("deviceid",)


def page_label(page: dict) -> str:
    """下拉里显示的一行：标题 + 去掉 query 的 URL。"""
    url = page.get("url") or ""
    short = url.split("?", 1)[0]
    if len(short) > 54:
        short = short[:26] + "…" + short[-24:]
    return "%s　%s" % (page.get("title") or "(无标题)", short)


def default_page_index(pages: list[dict]) -> int:
    """默认挑最后一个「正经页面」：内部隐藏页（file:// / deviceid）不算。"""
    for i in range(len(pages) - 1, -1, -1):
        url = (pages[i].get("url") or "").lower()
        if url.startswith("file://") or any(m in url for m in NOISE_PAGE_MARKS):
            continue
        return i
    return max(0, len(pages) - 1)


def list_pages_safe() -> list[dict]:
    """取页面列表；CDP 不通就返回空表（界面不该因为没连上而崩）。"""
    try:
        return CdpClient().list_pages()
    except (OSError, CdpError, ValueError):
        return []


def cdp_mouse(client: CdpClient, mtype: str, x: float, y: float,
              button: str | None = None, click_count: int = 0) -> None:
    params: dict = {"type": mtype, "x": x, "y": y}
    if button:
        params["button"] = button
        params["clickCount"] = click_count
    client.call("Input.dispatchMouseEvent", params)


def cdp_hover(client: CdpClient, x: float, y: float, settle: float = 0.10) -> None:
    """先把指针移到目标点并等 hover 落下。

    不少页面的可点元素要 :hover / mouseenter 之后才渲染出来或挂上监听，
    直接 press 就什么都不发生（表现为「点多少次都没用，鼠标移过去才灵」）。
    """
    cdp_mouse(client, "mouseMoved", x, y)     # 触发 hover / mouseenter
    time.sleep(settle)                        # 等一帧渲染 + hover 过渡
    cdp_mouse(client, "mouseMoved", x, y)     # 再动一次，让 hover 状态落地
    time.sleep(0.03)


def cdp_click_at(client: CdpClient, x: float, y: float, kind: int) -> None:
    """与 DLL CdpClickAt 同一序列：moved -> press/release（双击第二对 clickCount=2）。"""
    cdp_mouse(client, "mouseMoved", x, y)
    time.sleep(0.02)                      # DLL Sleep(20)：等悬停落下
    button = "right" if kind == 2 else "left"
    cdp_mouse(client, "mousePressed", x, y, button, 1)
    cdp_mouse(client, "mouseReleased", x, y, button, 1)
    if kind == 1:                         # 双击
        time.sleep(DBLCLK_GAP_MS / 1000.0)
        cdp_mouse(client, "mousePressed", x, y, button, 2)
        cdp_mouse(client, "mouseReleased", x, y, button, 2)


# --- 坐标拾取（全局低级鼠标钩子）----------------------------------------
class ClickPicker:
    """拾取一次落点在 view 上的左键点击，并把这次点击吞掉（应用不会响应）。"""

    def __init__(self, view: int, scale: float, out: queue.Queue):
        self.view = view
        self.scale = scale
        self.out = out
        self._proc = HOOKPROC(self._hook_proc)   # 必须持有引用，否则回调被回收
        self._tid = None
        self._stop = threading.Event()
        self._swallow_up = False
        self._swallow_rup = False
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._tid:
            user32.PostThreadMessageW(self._tid, WM_QUIT, 0, 0)

    def alive(self) -> bool:
        return self._thread.is_alive() and not self._stop.is_set()

    def _run(self) -> None:
        self._tid = kernel32.GetCurrentThreadId()
        hook = user32.SetWindowsHookExW(WH_MOUSE_LL, self._proc, None, 0)
        if not hook:
            self.out.put(("error", ctypes.get_last_error()))
            return
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        user32.UnhookWindowsHookEx(hook)

    def _hook_proc(self, n_code, w_param, l_param):
        if n_code >= 0:
            if w_param == WM_LBUTTONDOWN:
                info = ctypes.cast(l_param, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
                pt = wintypes.POINT(info.pt.x, info.pt.y)
                user32.ScreenToClient(self.view, ctypes.byref(pt))
                rect = wintypes.RECT()
                user32.GetClientRect(self.view, ctypes.byref(rect))
                if 0 <= pt.x < rect.right and 0 <= pt.y < rect.bottom:
                    self._swallow_up = True
                    self.out.put(("pick", pt.x, pt.y,
                                  round(pt.x / self.scale, 1), round(pt.y / self.scale, 1)))
                    self.stop()
                    return 1
            elif w_param == WM_LBUTTONUP and self._swallow_up:
                self._swallow_up = False
                return 1
            elif w_param == WM_RBUTTONDOWN:
                # 右键 = 取消这次拾取；同样吞掉，免得应用弹出右键菜单
                self._swallow_rup = True
                self.out.put(("cancel",))
                self.stop()
                return 1
            elif w_param == WM_RBUTTONUP and self._swallow_rup:
                self._swallow_rup = False
                return 1
        return user32.CallNextHookEx(None, n_code, w_param, l_param)


# --- 拾取遮罩 --------------------------------------------------------------
def fmt_coord(v) -> str:
    v = round(float(v), 1)
    return str(int(v)) if v.is_integer() else str(v)


class PickOverlay(tk.Toplevel):
    """拾取期间盖在目标 view 上的透明遮罩：十字线 + 实时鼠标坐标。

    底色用 -transparentcolor 抠成全透明，所以目标界面照常可见；
    再加 WS_EX_TRANSPARENT 让鼠标事件穿透，点下去仍然落在目标窗口上
    （点击本身由 ClickPicker 的低级钩子吞掉）。
    """

    BG = "black"          # 透明色：画布底色，绘制元素别用纯黑
    IN = "#00E5FF"        # 指针在 view 内：青色
    OUT = "#FF8A80"       # 指针在 view 外：红色（这时的点击不会被拾取）

    def __init__(self, master, view: int, scale: float):
        super().__init__(master)
        self.view = view
        self.scale = scale
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.attributes("-transparentcolor", self.BG)
        self.attributes("-alpha", 0.92)
        self.canvas = tk.Canvas(self, background=self.BG, highlightthickness=0,
                                borderwidth=0)
        self.canvas.pack(fill="both", expand=True)
        self._border = self.canvas.create_rectangle(0, 0, 0, 0, outline=self.IN, width=2)
        self._vline = self.canvas.create_line(0, 0, 0, 0, fill=self.IN)
        self._hline = self.canvas.create_line(0, 0, 0, 0, fill=self.IN)
        self._ring = self.canvas.create_rectangle(0, 0, 0, 0, outline=self.OUT, width=2)
        self._tag_bg = self.canvas.create_rectangle(0, 0, 0, 0, fill="#101820",
                                                    outline=self.IN)
        self._tag = self.canvas.create_text(0, 0, anchor="nw", fill="#FFFFFF",
                                            font=("Consolas", 11, "bold"), text="")
        self._tip = self.canvas.create_text(0, 0, anchor="n", fill="#FFD54F",
                                            font=("Microsoft YaHei UI", 10, "bold"),
                                            text="左键拾取　右键取消")
        self._place()
        self._click_through()
        self.refresh()

    def _place(self) -> None:
        """贴到 view 客户区（每次刷新都重贴，目标窗口挪动也能跟上）。"""
        rect = wintypes.RECT()
        user32.GetClientRect(self.view, ctypes.byref(rect))
        w = rect.right - rect.left
        h = rect.bottom - rect.top
        origin = wintypes.POINT(rect.left, rect.top)
        user32.ClientToScreen(self.view, ctypes.byref(origin))
        self.geometry("%dx%d+%d+%d" % (w, h, origin.x, origin.y))
        self.canvas.configure(width=w, height=h)
        self.canvas.coords(self._border, 1, 1, w - 2, h - 2)
        self.canvas.coords(self._tip, w // 2, 6)

    def _click_through(self) -> None:
        self.update_idletasks()
        self.update()                      # 先把窗口真正映射出来，样式才不会被 Tk 覆盖
        hwnd = self.winfo_id()
        if not hwnd:
            return
        style = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                                 style | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
                                 | WS_EX_NOACTIVATE)
        # 置顶但不抢焦点（Tk 的 -topmost 会激活窗口，这里补一次）
        user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)

    def refresh(self) -> None:
        """按当前鼠标位置重画（调用方按 ~30ms 轮询）。"""
        if not user32.IsWindow(self.view):
            return
        self._place()
        rect = wintypes.RECT()
        user32.GetClientRect(self.view, ctypes.byref(rect))
        w, h = rect.right, rect.bottom
        screen = wintypes.POINT()
        if not user32.GetCursorPos(ctypes.byref(screen)):
            return
        cur = wintypes.POINT(screen.x, screen.y)
        user32.ScreenToClient(self.view, ctypes.byref(cur))
        px, py = cur.x, cur.y
        inside = 0 <= px < w and 0 <= py < h
        color = self.IN if inside else self.OUT
        lx, ly = round(px / self.scale, 1), round(py / self.scale, 1)

        self.canvas.coords(self._vline, px, 0, px, h)
        self.canvas.coords(self._hline, 0, py, w, py)
        self.canvas.coords(self._ring, px - 5, py - 5, px + 5, py + 5)
        for item in (self._vline, self._hline):
            self.canvas.itemconfigure(item, fill=color)
        for item in (self._ring, self._border):
            self.canvas.itemconfigure(item, outline=color)
        self.canvas.itemconfigure(self._tag_bg, outline=color)

        text = "逻辑 (%s, %s)   物理 (%d, %d)" % (fmt_coord(lx), fmt_coord(ly), px, py)
        if not inside:
            text = "窗口外（点了不算）  " + text
        self.canvas.itemconfigure(self._tag, text=text)
        x, y = px + 14, py + 14
        self.canvas.coords(self._tag, x, y)
        box = self.canvas.bbox(self._tag)
        if box:
            x0, y0, x1, y1 = box
            # 贴边就往回挪，别让坐标牌跑出遮罩
            dx = min(0, (w - 4) - x1)
            dy = min(0, (h - 4) - y1)
            if dx or dy:
                self.canvas.coords(self._tag, x + dx, y + dy)
                box = self.canvas.bbox(self._tag)
                x0, y0, x1, y1 = box
            self.canvas.coords(self._tag_bg, x0 - 4, y0 - 3, x1 + 4, y1 + 3)

    def close(self) -> None:
        try:
            self.destroy()
        except tk.TclError:
            pass


# --- 事件编辑对话框 -------------------------------------------------------
class EventDialog(tk.Toplevel):
    FIELDS = ("x", "y", "click", "delay_ms", "wheel", "selector", "page", "remark")

    def __init__(self, master, event: dict, on_pick, on_cancel=None,
                 on_test_selector=None, title: str = "事件", pages=None):
        super().__init__(master)
        self.title(title)
        self.pages: list[dict] = list(pages or [])
        self._page0 = str(event.get("page") or "")     # 本行已存的页面
        # axure 稿里对话框只有 坐标/动作/备注，选择器与页面不再出现在界面上；
        # 编辑老行时把原值原样带回去，避免静默丢掉脚本里已有的字段。
        self._orig_selector = str(event.get("selector") or "").strip()
        self.resizable(False, False)
        self.result = None
        self._on_pick = on_pick
        self._on_cancel = on_cancel
        self._on_test_selector = on_test_selector

        # X/Y 就是本行的执行坐标（每行独立，改一行不影响其他行）。
        self.x_var = tk.StringVar(value=self._fmt(event.get("x", 0)))
        self.y_var = tk.StringVar(value=self._fmt(event.get("y", 0)))
        self.click_var = tk.StringVar(value=CLICK_LABELS.get(event.get("click", "left"), "左键单击"))
        self.delay_var = tk.StringVar(value=str(event.get("delay_ms", 0)))
        self.wheel_var = tk.StringVar(value=str(event.get("wheel", 0)))
        self.note_var = tk.StringVar(value=event.get("remark", "") or "")

        body = ttk.Frame(self, padding=(12, 10))
        body.grid(row=0, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)

        vcmd = (self.register(self._num_ok), "%P")
        label_w = 9

        def add_row(parent, row, label, var=None, width=10, suffix="", combo_values=None):
            """一行 = 标签 + 输入控件 + 灰色单位/提示，用 pack 保证提示紧跟输入框。"""
            line = ttk.Frame(parent)
            line.grid(row=row, column=0, columnspan=2, sticky="ew", pady=3)
            ttk.Label(line, text=label, width=label_w, anchor="w").pack(side="left")
            if combo_values is not None:
                widget = ttk.Combobox(line, textvariable=var, width=width,
                                      state="readonly", values=combo_values)
            else:
                widget = ttk.Entry(line, textvariable=var, width=width,
                                   validate="key", validatecommand=vcmd)
            widget.pack(side="left")
            if suffix:
                ttk.Label(line, text=suffix, foreground="#777").pack(side="left", padx=(8, 0))
            return widget

        # 坐标：X/Y 两行 + 右侧一个跨两行的拾取按钮
        pos = ttk.LabelFrame(body, text="坐标（FLUTTERVIEW 逻辑 px，96 dpi）", padding=(10, 4))
        pos.grid(row=0, column=0, sticky="ew")
        pos.columnconfigure(0, weight=1)
        self.x_entry = add_row(pos, 0, "X", self.x_var)
        self.y_entry = add_row(pos, 1, "Y", self.y_var)
        self.pick_xy_btn = ttk.Button(pos, text="拾取坐标", width=12, command=self._pick_xy)
        self.pick_xy_btn.grid(row=0, column=2, rowspan=2, sticky="nsew",
                              padx=(10, 0), pady=3)

        # 动作
        act = ttk.LabelFrame(body, text="动作", padding=(10, 4))
        act.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        act.columnconfigure(0, weight=1)
        add_row(act, 0, "点击事件", self.click_var,
                combo_values=list(CLICK_LABELS.values()))
        add_row(act, 1, "间隔时间", self.delay_var, suffix="ms（点完后等）")
        add_row(act, 2, "滚轮格数", self.wheel_var, suffix="格（负 = 向下）")

        # 底部按钮：确定在右，Enter/Esc 也能用（坐标 / 动作 两块，备注已从弹窗去掉——
        # 备注是给人看的、DLL 执行不读，放弹窗里是多余输入）
        foot = ttk.Frame(body)
        foot.grid(row=2, column=0, sticky="ew", pady=(12, 0))
        foot.columnconfigure(0, weight=1)
        ttk.Label(foot, text="Enter 确定　Esc 取消", foreground="#888").grid(
            row=0, column=0, sticky="w")
        ttk.Button(foot, text="确定", width=10, command=self._ok).grid(
            row=0, column=1, padx=(0, 6))
        ttk.Button(foot, text="取消", width=10, command=self.destroy).grid(row=0, column=2)

        self.resizable(True, False)
        self.transient(master)
        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.bind("<Return>", lambda _e: self._ok())
        self.bind("<Escape>", lambda _e: self.destroy())
        self.x_entry.focus_set()
        self.x_entry.selection_range(0, "end")

        # 备注要求：弹窗居中显示在父窗口正中
        self.update_idletasks()
        mw = self.master.winfo_width() or self.master.winfo_screenwidth()
        mh = self.master.winfo_height() or self.master.winfo_screenheight()
        mx = self.master.winfo_rootx()
        my = self.master.winfo_rooty()
        self.geometry("+%d+%d" % (mx + (mw - self.winfo_width()) // 2,
                                  my + (mh - self.winfo_height()) // 2))

    @staticmethod
    def _num_ok(text: str) -> bool:
        """输入框校验：只放行能拼成数字的中间态（空 / - / . / -1.5）。"""
        if text in ("", "-", ".", "-."):
            return True
        try:
            float(text)
        except ValueError:
            return False
        return True

    @staticmethod
    def _fmt(v: float) -> str:
        v = round(float(v), 1)
        return str(int(v)) if v.is_integer() else str(v)

    def _pick_xy(self):
        def fill(lx, ly):
            if not self.winfo_exists():
                return
            self.x_var.set(self._fmt(lx))
            self.y_var.set(self._fmt(ly))
        self._on_pick(fill)

    def destroy(self):
        # 关窗/取消时把可能还在跑的拾取钩子一并收掉
        if self._on_cancel is not None:
            try:
                self._on_cancel()
            except Exception:
                pass
            finally:
                self._on_cancel = None
        try:
            super().destroy()
        except tk.TclError:
            pass

    def _ok(self):
        try:
            event = {
                "x": float(self.x_var.get()),
                "y": float(self.y_var.get()),
                "click": LABEL_CLICKS.get(self.click_var.get(), "left"),
                "delay_ms": int(float(self.delay_var.get() or 0)),
                "wheel": int(float(self.wheel_var.get() or 0)),
                "selector": self._orig_selector,
                "page": self._page0,
                "remark": self.note_var.get().strip(),
            }
        except ValueError:
            messagebox.showerror("格式错误", "坐标/间隔/滚轮必须是数字", parent=self)
            return
        for key in ("x", "y"):
            if float(event[key]).is_integer():
                event[key] = int(event[key])
        self.result = event
        self.destroy()


# --- 页面结构窗口 ---------------------------------------------------------
class DomWindow(tk.Toplevel):
    """把 WebView 页面的 DOM 摊成一张表，方便挑选择器（CDP 读一次，离线筛选）。"""

    COLUMNS = (
        ("el", "元素（按层级缩进）", 240, "w"),
        ("sel", "可用选择器", 250, "w"),
        ("tx", "文本", 180, "w"),
        ("pos", "位置 x,y　大小", 190, "center"),
    )

    def __init__(self, master, on_fill=None, on_refresh=None, pages=None):
        super().__init__(master)
        self.title("页面结构")
        try:
            s = max(1.0, float(self.winfo_fpixels("1i")) / 96.0)
        except tk.TclError:
            s = 1.0
        self.geometry("%dx%d" % (int(940 * s), int(580 * s)))
        self.minsize(int(600 * s), int(320 * s))
        self.nodes: list[dict] = []
        self._on_fill = on_fill
        self._on_refresh = on_refresh
        self.pages: list[dict] = list(pages or [])

        body = ttk.Frame(self, padding=8)
        body.pack(fill="both", expand=True)

        head = ttk.Frame(body)
        head.pack(fill="x")
        # 宿主可能开了多个 WebView（真正的活动页 + 应用内部隐藏页），得能挑
        self.page_var = tk.StringVar(value="")
        self.page_combo = None
        if self.pages:
            ttk.Label(head, text="页面").pack(side="left")
            self.page_combo = ttk.Combobox(head, width=40, state="readonly",
                                           values=[page_label(p) for p in self.pages])
            self.page_combo.pack(side="left", padx=(4, 10))
            self.page_combo.current(default_page_index(self.pages))
            self.page_combo.bind("<<ComboboxSelected>>", lambda _e: self._refresh())
        ttk.Label(head, text="筛选").pack(side="left")
        self.filter_var = tk.StringVar()
        ttk.Entry(head, textvariable=self.filter_var, width=20).pack(side="left", padx=(4, 8))
        self.filter_var.trace_add("write", lambda *_: self._fill_tree())
        if self._on_refresh is not None:
            ttk.Button(head, text="刷新", width=6, command=self._refresh).pack(side="left")
        self.info_var = tk.StringVar(value="")
        ttk.Label(head, textvariable=self.info_var, foreground="#555").pack(side="left", padx=(10, 0))

        table = ttk.Frame(body)
        table.pack(fill="both", expand=True, pady=(6, 0))
        self.tree = ttk.Treeview(table, columns=[c[0] for c in self.COLUMNS],
                                 show="headings", selectmode="browse")
        for key, title, width, anchor in self.COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor=anchor,
                             stretch=(key in ("el", "sel", "tx")))
        yscroll = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=yscroll.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        table.rowconfigure(0, weight=1)
        table.columnconfigure(0, weight=1)
        self.tree.bind("<Double-1>", lambda _e: self.copy_selector())

        foot = ttk.Frame(body)
        foot.pack(fill="x", pady=(6, 0))
        foot.columnconfigure(0, weight=1)
        self.tip_var = tk.StringVar(value="双击一行 = 复制选择器到剪贴板")
        ttk.Label(foot, textvariable=self.tip_var, foreground="#777").grid(
            row=0, column=0, sticky="w")
        ttk.Button(foot, text="复制选择器", width=12, command=self.copy_selector).grid(
            row=0, column=1, padx=(6, 0))
        if self._on_fill is not None:
            ttk.Button(foot, text="填入选中行", width=12, command=self.fill_selected).grid(
                row=0, column=2, padx=(6, 0))
        ttk.Button(foot, text="关闭", width=8, command=self.destroy).grid(
            row=0, column=3, padx=(6, 0))

    # -- 页面 ---------------------------------------------------------------
    def current_match(self) -> str:
        """当前选中页面的完整 URL（拿去当 CDP 选页的子串）。"""
        idx = -1
        if self.page_combo is not None:
            try:
                idx = self.page_combo.current()
            except tk.TclError:
                idx = -1
        if 0 <= idx < len(self.pages):
            return self.pages[idx].get("url") or ""
        return ""

    def _refresh(self) -> None:
        if self._on_refresh is not None:
            self._on_refresh(self.current_match())

    # -- 数据 ---------------------------------------------------------------
    def load(self, data: dict) -> None:
        self.nodes = list(data.get("items") or [])
        url = data.get("url") or ""
        self.title("页面结构 — %s" % (data.get("title") or url or "?"))
        self.info_var.set("视口 %sx%s　节点 %d%s"
                          % (data.get("vw", "?"), data.get("vh", "?"), data.get("n", 0),
                             "（只取前 400 个）" if data.get("n", 0) >= 400 else ""))
        self._fill_tree()

    @staticmethod
    def _match(node: dict, key: str) -> bool:
        if not key:
            return True
        low = key.lower()
        return (low in (node.get("tag") or "") or low in (node.get("id") or "")
                or low in (node.get("cls") or "") or low in (node.get("tx") or "")
                or low in (node.get("sel") or ""))

    def _fill_tree(self) -> None:
        self.tree.delete(*self.tree.get_children())
        key = self.filter_var.get().strip()
        shown = 0
        for i, node in enumerate(self.nodes):
            if not self._match(node, key):
                continue
            shown += 1
            indent = "　" * max(0, int(node.get("d", 1)) - 1)
            tag = node.get("tag") or ""
            label = indent + tag + ("#" + node["id"] if node.get("id") else "")
            rect = "%s,%s　%s×%s" % (node.get("x", "?"), node.get("y", "?"),
                                     node.get("w", "?"), node.get("h", "?"))
            self.tree.insert("", "end", iid=str(i), values=(
                label, node.get("sel") or "", node.get("tx") or "", rect))
        self.tip_var.set("显示 %d / %d 个节点（双击 = 复制选择器）"
                         % (shown, len(self.nodes)))

    def _selected_selector(self) -> str:
        sel = self.tree.selection()
        if not sel:
            return ""
        try:
            return str(self.nodes[int(sel[0])].get("sel") or "")
        except (ValueError, IndexError):
            return ""

    def copy_selector(self) -> None:
        value = self._selected_selector()
        if not value:
            self.tip_var.set("先点一行，再复制")
            return
        self.clipboard_clear()
        self.clipboard_append(value)
        self.tip_var.set("已复制：%s" % value)

    def fill_selected(self) -> None:
        value = self._selected_selector()
        if not value:
            self.tip_var.set("先点一行，再填入")
            return
        if self._on_fill is not None:
            self._on_fill(value, self.current_match())


# --- 主窗口 ---------------------------------------------------------------
DEFAULT_EVENTS = [
    {"x": 27, "y": 113, "click": "left", "delay_ms": 600,
     "wheel": 0, "selector": "", "remark": ""},
    {"x": 1573, "y": 87, "click": "left", "delay_ms": 0,
     "wheel": 0, "selector": "", "remark": ""},
]


class App:
    COLUMNS = (
        ("idx", "序号", 44, "center"),
        ("pos", "坐标", 96, "center"),
        ("click", "行为", 104, "center"),
        ("delay", "间隔(ms)", 126, "center"),
        ("wheel", "滚轮", 122, "center"),
        ("note", "备注", 234, "w"),
    )

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("pplugin 点击脚本编辑器")
        # Tk 的字体按 DPI 放大（150% 屏 scaling=2.0），几何/列宽也要同步放大，
        # 否则文字放大了而窗口没放大，布局会挤成一团。
        self.scale = self._ui_scale()
        self.root.geometry("%dx%d" % (int(748 * self.scale), int(725 * self.scale)))
        self.root.minsize(int(700 * self.scale), int(640 * self.scale))

        self.scripts_dir = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "scripts")
        os.makedirs(self.scripts_dir, exist_ok=True)

        self.target = Target()
        self.events: list[dict] = [dict(e) for e in DEFAULT_EVENTS]
        self.current_name = ""
        self.dirty = True

        self.pick_queue: queue.Queue | None = None
        self.picker: ClickPicker | None = None
        self.pick_cb = None
        self.overlay: PickOverlay | None = None    # 拾取期间盖在目标窗口上的透明遮罩
        self._cursor_job = None

        self.msg_queue: queue.Queue = queue.Queue()
        self.stop_evt = threading.Event()
        self.running = False
        self.follow_var = tk.BooleanVar(value=False)   # 只给勾选项用（Tk 线程）
        self.follow_on = False                         # 后台线程只读这个普通变量
        self.script_view_size: tuple[int, int] | None = None
        self.pages: list[dict] = []                  # /json/list 的 WebView 页面
        # 后台消息：选择器行用 PostMessage 往 view 投鼠标消息，不动光标不抢前台
        self.bg_msg_var = tk.BooleanVar(value=True)       # 勾选项（Tk 线程）
        self._bg_msg = True                               # 后台线程只读这个
        self._page_origin: tuple[float, float] | None = None   # 页面 px → view px 偏移

        self.name_var = tk.StringVar(value="活动")
        self.hotkey_var = tk.StringVar(value="")
        self.loop_var = tk.StringVar(value="1")
        self.status_var = tk.StringVar(value="")

        # 一次试跑共用一条 CDP 连接；失败一次后本轮不再重试（与 DLL 一致）
        self._cdp: CdpClient | None = None
        self._cdp_page = ""                      # 当前连接对应的页面（换页要重连）
        self._cdp_failed = False
        self._cdp_target = ""                    # 脚本默认页（没写 page 的行用它）

        self._build()
        self.refresh_target(quiet=True)
        self._status(self.target.describe())
        self.refresh_script_list(load_first=True)
        self.render_table()
        self._log_dll_view()          # 一开窗就说清 DLL 现在会加载哪份、收不收
        self.root.after(100, self._poll_messages)
        threading.Thread(target=self._follow_thread, daemon=True).start()

    def _ui_scale(self) -> float:
        try:
            return max(1.0, float(self.root.winfo_fpixels("1i")) / 96.0)
        except tk.TclError:
            return 1.0

    # -- UI ----------------------------------------------------------------
    def _init_style(self) -> None:
        """行高/表头跟着 DPI 放大，否则 125%/150% 屏幕上表格文字会挤在一起。"""
        style = ttk.Style(self.root)
        base = "Microsoft YaHei UI"
        style.configure("Treeview", font=(base, 9), rowheight=int(24 * self.scale))
        style.configure("Treeview.Heading", font=(base, 9, "bold"))
        self.tree_tag_odd = "odd"
        self.tree_tag_even = "even"

    def _build(self) -> None:
        self._init_style()

        outer = ttk.Frame(self.root, padding=8)
        outer.pack(fill="both", expand=True)

        # --- 顶部：脚本列表 + 操作（Axure 稿：脚本列表下拉 + 刷新/新建/保存/删除）----
        top = self.top = ttk.Frame(outer)
        top.pack(fill="x")
        top.columnconfigure(0, weight=1)

        files = ttk.LabelFrame(top, text="脚本列表", padding=(8, 4))
        files.grid(row=0, column=0, sticky="nsew")
        self.script_combo = ttk.Combobox(files, width=16, state="readonly")
        self.script_combo.pack(side="left", padx=(4, 6))
        self.script_combo.bind("<<ComboboxSelected>>", self._on_script_selected)
        ttk.Button(files, text="刷新", width=6, command=self.refresh_script_list).pack(side="left")
        ttk.Button(files, text="新建", width=6, command=self.new_script).pack(side="left", padx=(4, 0))
        ttk.Button(files, text="保存", width=6, command=self.save_script).pack(side="left", padx=(4, 0))
        ttk.Button(files, text="删除", width=6, command=self.delete_script).pack(side="left", padx=(4, 0))


        # --- 底部操作条（Axure 稿：试跑/选中试跑/停止 | 清日志/刷新应用）----
        bottom = self.bottom = ttk.Frame(outer, padding=(0, 6))
        bottom.pack(side="bottom", fill="x")
        self.run_btn = ttk.Button(bottom, text="试跑", width=6, command=self.do_run)
        self.run_btn.pack(side="left")
        ttk.Button(bottom, text="选中试跑", width=9, command=self.do_run_selected).pack(
            side="left", padx=(4, 0))
        self.stop_btn = ttk.Button(bottom, text="停止", width=6, command=self.do_stop,
                                   state="disabled")
        self.stop_btn.pack(side="left", padx=(4, 10))
        ttk.Separator(bottom, orient="vertical").pack(side="left", fill="y", padx=(0, 10))
        ttk.Button(bottom, text="清日志", width=7, command=self._clear_log).pack(side="left")
        ttk.Button(bottom, text="刷新应用", width=9, command=self.refresh_target).pack(
            side="left", padx=(4, 0))
        ttk.Label(bottom, textvariable=self.status_var, foreground="#555",
                  anchor="e").pack(side="right", padx=(12, 0))

        # --- 中间：可拖拽分隔的「事件表 / 执行日志」------------------------
        paned = self.paned = ttk.Panedwindow(outer, orient="vertical")
        paned.pack(fill="both", expand=True)

        table_box = self.table_frame = ttk.LabelFrame(paned, text="事件表", padding=(6, 4))
        paned.add(table_box, weight=4)
        tools = ttk.Frame(table_box)
        tools.pack(fill="x", pady=(0, 4))
        ttk.Button(tools, text="添加事件", command=self.add_manual).pack(side="left")
        ttk.Button(tools, text="修改", width=6, command=self.edit_selected).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="删除行", width=7, command=self.delete_selected).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="上移", width=6, command=lambda: self.move_row(-1)).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="下移", width=6, command=lambda: self.move_row(1)).pack(side="left", padx=(4, 0))
        self.count_var = tk.StringVar(value="")
        self.count_lbl = ttk.Label(tools, textvariable=self.count_var, foreground="#555")
        self.count_lbl.pack(side="right")

        table = ttk.Frame(table_box)
        table.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(table, columns=[c[0] for c in self.COLUMNS], show="headings",
                                 selectmode="browse")
        for key, title, width, anchor in self.COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=int(width * self.scale), minwidth=int(40 * self.scale),
                             anchor=anchor, stretch=(key == "note"))
        yscroll = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        xscroll = ttk.Scrollbar(table, orient="horizontal", command=self.tree.xview)
        self.tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")
        table.rowconfigure(0, weight=1)
        table.columnconfigure(0, weight=1)
        self.tree.tag_configure(self.tree_tag_odd, background="#F6F7F9")
        self.tree.tag_configure(self.tree_tag_even, background="#FFFFFF")
        self.tree.bind("<Double-1>", lambda _e: self.edit_selected())
        self.tree.bind("<Delete>", lambda _e: self.delete_selected())
        self.tree.bind("<Return>", lambda _e: self.edit_selected())

        # 执行日志：跑脚本时逐行输出 DLL 同字样的痕迹，[DLL] 前缀的是
        # DLL 自己写的行，两边可以直接逐行比对。
        logf = self.logf = ttk.LabelFrame(
            paned, text="脚本日志", padding=(6, 4))
        paned.add(logf, weight=1)
        log_box_wrap = ttk.Frame(logf)
        log_box_wrap.pack(fill="both", expand=True)
        self.log_box = tk.Text(log_box_wrap, height=6, wrap="none", state="disabled",
                               font=("Consolas", 9), relief="flat",
                               background="#141414", foreground="#B3A981")
        log_scroll = ttk.Scrollbar(log_box_wrap, orient="vertical", command=self.log_box.yview)
        log_xscroll = ttk.Scrollbar(log_box_wrap, orient="horizontal", command=self.log_box.xview)
        self.log_box.configure(yscrollcommand=log_scroll.set, xscrollcommand=log_xscroll.set)
        self.log_box.grid(row=0, column=0, sticky="nsew")
        log_scroll.grid(row=0, column=1, sticky="ns")
        log_xscroll.grid(row=1, column=0, sticky="ew")
        log_box_wrap.rowconfigure(0, weight=1)
        log_box_wrap.columnconfigure(0, weight=1)
        self.log_box.tag_config("warn", foreground="#FFB454")
        self.log_box.tag_config("dll", foreground="#8FD3A0")

        # 表格占大头，日志留 5 行左右；之后用户可以自己拖分隔条
        self.root.after_idle(self._place_sash)

        self.root.bind("<Control-s>", lambda _e: self.save_script())
        self.root.bind("<Control-n>", lambda _e: self.new_script())
        self.root.bind("<F5>", lambda _e: self.do_run())
        for var in (self.name_var, self.hotkey_var, self.loop_var):
            var.trace_add("write", lambda *_: self._mark_dirty())
        self._update_title()
        self.root.after_idle(self._fit_minsize)

    def _fit_minsize(self) -> None:
        """最小宽度按顶部两栏的实际需要算——窗口太窄会把属性栏裁掉。"""
        self.root.update_idletasks()
        need = self.top.winfo_reqwidth() + 32
        # 小屏幕上别把最小宽度顶到屏幕外，超了就按屏幕宽度封顶
        avail = self.root.winfo_screenwidth() - 40
        self.root.minsize(max(int(820 * self.scale), min(need, avail)),
                          int(600 * self.scale))

    def _mark_dirty(self) -> None:
        self.dirty = True
        self._update_title()

    def _place_sash(self, tries: int = 0) -> None:
        """等窗格真正布局好再分配高度（太早调用量到 0，日志会被压没）。"""
        h = self.paned.winfo_height()
        if h < 240 and tries < 20:
            self.root.after(60, lambda: self._place_sash(tries + 1))
            return
        try:
            self.paned.sashpos(0, max(120, int(h * 0.64)))
        except tk.TclError:
            pass

    # -- 目标窗口 ----------------------------------------------------------
    def refresh_target(self, quiet: bool = False) -> bool:
        self.target = find_target()
        self._refresh_pages()
        if not quiet:
            self._status(self.target.describe())
        elif not self.target.ok:
            self._status(self.target.describe())
        return self.target.ok

    def _ensure_target(self) -> bool:
        if self.target.ok:
            return True
        self.refresh_target(quiet=True)
        if not self.target.ok:
            messagebox.showwarning("找不到 pplugin",
                                   "没找到 FLUTTERVIEW 子窗口。先启动 TT语音，再点「刷新目标」。")
            return False
        return True

    # -- 页面（每行可指定走哪张 WebView）-----------------------------------
    def _refresh_pages(self) -> None:
        """拉一次 /json/list；页面结构与「按行选页」都用它。"""
        self.pages = list_pages_safe()

    def _page_short(self, url: str) -> str:
        """表格里显示的页面名：能对上就显示标题，否则显示 URL 尾巴。"""
        if not url:
            return "默认"
        for page in self.pages:
            if (page.get("url") or "") == url:
                return page.get("title") or url.split("?", 1)[0].rsplit("/", 1)[-1]
        short = url.split("?", 1)[0]
        return ("…" + short[-26:]) if len(short) > 28 else short

    # -- 表格 --------------------------------------------------------------
    def render_table(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, ev in enumerate(self.events, 1):
            sel = str(ev.get("selector") or "").strip()
            self.tree.insert("", "end", iid=str(i - 1),
                             tags=(self.tree_tag_odd if i % 2 else self.tree_tag_even,),
                             values=(
                i,
                "%s, %s" % (self._num(ev.get("x", 0)), self._num(ev.get("y", 0))),
                CLICK_LABELS.get(ev.get("click", "left"), "左键单击"),
                ev.get("delay_ms", 0),
                ev.get("wheel", 0),
                ev.get("remark", "") or "",
            ))
        over = len(self.events) > MAX_EVENTS
        self.count_var.set("共 %d 行%s" % (len(self.events),
                                          "（超过 DLL 上限 %d，只有前 %d 行会跑）"
                                          % (MAX_EVENTS, MAX_EVENTS) if over else ""))
        self.count_lbl.configure(foreground="#C0392B" if over else "#555")
        self._update_title()

    def _update_title(self) -> None:
        """标题带脚本名 + 未保存标记，切窗口时一眼看出改没改。"""
        name = self.current_name or self.name_var.get().strip() or "未命名"
        self.root.title("脚本编辑器 — %s%s" % (name, " *" if self.dirty else ""))

    @staticmethod
    def _num(value) -> str:
        try:
            return str(int(value)) if float(value).is_integer() else str(value)
        except (TypeError, ValueError):
            return str(value)

    def _selected_index(self):
        sel = self.tree.selection()
        return int(sel[0]) if sel else None

    def _add_event(self, event: dict) -> None:
        self.events.append(event)
        self.dirty = True
        self.render_table()
        self.tree.selection_set(str(len(self.events) - 1))
        self.tree.see(str(len(self.events) - 1))

    def _last_xy(self) -> tuple[float, float]:
        """最后一行的坐标，新加行拿来当默认值；没有行时为 (0, 0)。"""
        if not self.events:
            return (0.0, 0.0)
        ev = self.events[-1]
        return (float(ev.get("x", 0) or 0), float(ev.get("y", 0) or 0))

    def _normalize_events(self) -> None:
        """统一成新格式：x/y 就是本行执行坐标（老脚本的 ox/oy 折算进来后删掉），note→remark。"""
        for ev in self.events:
            if "remark" not in ev:
                ev["remark"] = str(ev.pop("note", "") or "")
            # 老格式：x/y 是基准、ox/oy 是本行偏移 —— 折算成绝对坐标后删掉偏移
            ox = ev.pop("ox", 0) or 0
            oy = ev.pop("oy", 0) or 0
            ev["x"] = self._shift(ev.get("x", 0), ox)
            ev["y"] = self._shift(ev.get("y", 0), oy)
            # page：本行走哪张 WebView 页（空 = 跟随脚本默认页）
            ev["page"] = str(ev.get("page") or "").strip()

    def add_manual(self) -> None:
        bx, by = self._last_xy()
        ev = {"x": bx, "y": by, "click": "left",
              "delay_ms": 0, "wheel": 0, "selector": "", "page": "", "remark": ""}
        dialog = EventDialog(self.root, ev, self.start_pick, self._end_pick,
                             on_test_selector=self.test_selector, title="新增事件",
                             pages=list_pages_safe())
        self.root.wait_window(dialog)
        if dialog.result:
            self._add_event(dialog.result)

    def edit_selected(self) -> None:
        idx = self._selected_index()
        if idx is None:
            self._status("先选中一行")
            return
        ev = dict(self.events[idx])
        dialog = EventDialog(self.root, ev, self.start_pick, self._end_pick,
                             on_test_selector=self.test_selector,
                             title="修改第 %d 行" % (idx + 1),
                             pages=list_pages_safe())
        self.root.wait_window(dialog)
        if dialog.result:
            # 只替换这一行，其余行原样不动
            self.events[idx] = dialog.result
            self.dirty = True
            self.render_table()
            self.tree.selection_set(str(idx))

    @staticmethod
    def _shift(value, delta) -> float:
        v = round(float(value or 0) + float(delta), 1)
        return int(v) if v.is_integer() else v

    def delete_selected(self) -> None:
        idx = self._selected_index()
        if idx is None:
            return
        del self.events[idx]
        self.dirty = True
        self.render_table()
        if self.events:
            self.tree.selection_set(str(min(idx, len(self.events) - 1)))

    def move_row(self, delta: int) -> None:
        idx = self._selected_index()
        if idx is None:
            return
        new = idx + delta
        if not 0 <= new < len(self.events):
            return
        self.events[idx], self.events[new] = self.events[new], self.events[idx]
        self.dirty = True
        self.render_table()
        self.tree.selection_set(str(new))

    # -- 拾取 --------------------------------------------------------------
    def start_pick(self, on_point) -> None:
        if self.picker is not None:
            return
        if not self._ensure_target():
            return
        self.pick_cb = on_point
        self.pick_queue = queue.Queue()
        self.picker = ClickPicker(self.target.view, self.target.scale, self.pick_queue)
        self.picker.start()
        self._show_overlay()
        self._status("拾取中：左键点要拾的位置，右键取消（遮罩上会显示当前坐标）")
        self.root.after(80, self._poll_pick)

    # -- 拾取遮罩 ----------------------------------------------------------
    def _show_overlay(self) -> None:
        """在目标窗口上盖一层透明遮罩，实时显示鼠标当前位置。"""
        self._hide_overlay()
        if not self.target.ok:
            return
        try:
            self.overlay = PickOverlay(self.root, self.target.view, self.target.scale)
        except Exception as exc:          # 遮罩只是辅助，失败不能挡住拾取
            self.overlay = None
            self._status("遮罩创建失败（照常拾取）：%s" % exc)
            return
        # 遮罩窗口会顺手抢走前台，这里把它还给目标窗口
        if self.target.main:
            user32.SetForegroundWindow(self.target.main)
        self._poll_cursor()

    def _poll_cursor(self) -> None:
        """~30ms 刷一次十字线/坐标，跟鼠标手感接近。"""
        if self.overlay is None:
            return
        try:
            self.overlay.refresh()
        except tk.TclError:
            self._hide_overlay()
            return
        if self.overlay is not None:
            self._cursor_job = self.root.after(30, self._poll_cursor)

    def _hide_overlay(self) -> None:
        if self._cursor_job is not None:
            try:
                self.root.after_cancel(self._cursor_job)
            except tk.TclError:
                pass
            self._cursor_job = None
        if self.overlay is not None:
            self.overlay.close()
            self.overlay = None

    def _poll_pick(self) -> None:
        if self.pick_queue is None:
            return
        try:
            kind, *rest = self.pick_queue.get_nowait()
        except queue.Empty:
            if self.picker is not None and self.picker.alive():
                self.root.after(80, self._poll_pick)
            else:
                self._end_pick()
            return

        if kind == "error":
            self._status("钩子安装失败 err=%s" % (rest[0] if rest else "?"))
            self._end_pick()
            return

        if kind == "cancel":      # 遮罩上右键 = 放弃这次拾取
            self._status("已取消拾取（右键）")
            self._end_pick()
            return

        px, py, lx, ly = rest
        self._status("拾取到 物理(%s,%s) → 逻辑(%s,%s)" % (px, py, lx, ly))
        cb = self.pick_cb
        self._end_pick()
        if cb:
            cb(lx, ly)

    def _end_pick(self) -> None:
        self._hide_overlay()
        if self.picker is not None:
            self.picker.stop()
        self.picker = None
        self.pick_queue = None
        self.pick_cb = None

    # -- 文件 --------------------------------------------------------------
    def refresh_script_list(self, load_first: bool = False) -> None:
        names = []
        if os.path.isdir(self.scripts_dir):
            for name in sorted(os.listdir(self.scripts_dir)):
                if name.lower().endswith(".json"):
                    names.append(name[:-5])
        self.script_combo["values"] = names
        if not names:
            self.current_name = ""
            self.script_combo.set("")
            return
        if load_first and not self.current_name:
            self.load_script(names[0])

    def _on_script_selected(self, _event=None) -> None:
        name = self.script_combo.get()
        if name and name != self.current_name:
            self.load_script(name)

    def _script_path(self, name: str) -> str:
        return os.path.join(self.scripts_dir, name + ".json")

    def load_script(self, name: str) -> None:
        path = self._script_path(name)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError) as exc:
            messagebox.showerror("读取失败", "%s\n%s" % (path, exc))
            return
        self.current_name = name
        self.name_var.set(data.get("name", name))
        self.hotkey_var.set(data.get("hotkey", ""))
        self.loop_var.set(str(data.get("loop", 1)))
        self.events = [dict(e) for e in data.get("events", [])]
        self._normalize_events()
        size = data.get("view_size") or []
        self.script_view_size = (int(size[0]), int(size[1])) if len(size) == 2 else None
        self.dirty = False
        self.script_combo.set(name)
        self.render_table()
        self._status("已载入 %s（%d 行）" % (name, len(self.events)))
        self._log("file: %s" % path)

    def _collect(self) -> dict:
        w, h = self.target.w, self.target.h
        events = []
        for ev in self.events:
            item = dict(ev)
            item.pop("text", None)  # 粘贴功能已移除，老脚本里的 text 一并清掉
            # 门控已整体移除，老脚本里的这两个键一并清掉（DLL 当未知键跳过）
            item.pop("wait_px", None)
            item.pop("wait_max_ms", None)
            # 偏移已废弃：老脚本残留的 ox/oy 折算进 x/y 后删掉，
            # 之后每行只写自己的绝对坐标（DLL 缺 ox/oy 时按 0 执行 x/y）
            ox = item.pop("ox", 0) or 0
            oy = item.pop("oy", 0) or 0
            item["x"] = self._shift(item.get("x", 0), ox)
            item["y"] = self._shift(item.get("y", 0), oy)
            # 老脚本没有 remark 键，保存时补齐，DLL 会 JsonSkip 掉
            item["remark"] = str(item.get("remark") or "").strip()
            # selector 同理补齐（空 = 本行走坐标点击），DLL 认这个键
            item["selector"] = str(item.get("selector") or "").strip()
            # page：本行要走的 WebView 页面（URL 子串）；空 = 跟随脚本默认页，
            # 空着就不写进 JSON，保持文件干净
            page = str(item.pop("page", "") or "").strip()
            if page:
                item["page"] = page
            events.append(item)
        data = {
            "version": 1,
            "name": self.name_var.get().strip() or self.current_name or "script",
            "hotkey": self.hotkey_var.get().strip(),
            "loop": min(MAX_LOOP, max(1, self._int(self.loop_var.get(), 1))),
            "events": events,
        }
        if w > 0 and h > 0:
            # DLL 不认这个键（JsonSkip 会跳过），只给编辑器做坐标基准提醒
            data["view_size"] = [w, h]
        return data

    @staticmethod
    def _int(text: str, default: int) -> int:
        try:
            return int(float(text))
        except (TypeError, ValueError):
            return default

    def new_script(self) -> None:
        if self.running:
            return
        self.current_name = ""
        self.script_combo.set("")
        self.name_var.set("新脚本")
        self.hotkey_var.set("")
        self.loop_var.set("1")
        self.events = []
        self.dirty = True
        self.render_table()
        self._status("新建脚本：加两行事件后点「保存」")

    def save_script(self) -> None:
        name = self.current_name or self.name_var.get().strip()
        if not name:
            self.save_script_as()
            return
        self._write(name)

    def save_script_as(self) -> None:
        name = simpledialog.askstring("另存为", "脚本名称：", initialvalue=self.name_var.get(),
                                      parent=self.root)
        if not name:
            return
        name = name.strip().replace("\\", "_").replace("/", "_")
        if not name:
            return
        self.current_name = name
        self.name_var.set(name)
        self._write(name)

    def _write(self, name: str) -> None:
        data = self._collect()
        path = self._script_path(name)
        try:
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(data, fh, ensure_ascii=False, indent=2)
                fh.write("\n")
        except OSError as exc:
            messagebox.showerror("保存失败", "%s\n%s" % (path, exc))
            return
        self.current_name = name
        self.dirty = False
        self.refresh_script_list()
        self.script_combo.set(name)
        self._write_active(name)
        if self.target.ok:
            self.script_view_size = (self.target.w, self.target.h)
        self._update_title()
        self._status("已保存 %s" % name)
        self._log("file: 已保存 %s" % path)
        self._warn_dll_limits()
        self._log_dll_view()

    def _write_active(self, name: str) -> None:
        """DLL 按这个文件选脚本（优先级高于最新 .json）。"""
        path = os.path.join(self.scripts_dir, "active.txt")
        try:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(name + "\n")
        except OSError:
            pass

    def _warn_dll_limits(self) -> None:
        """把 DLL 会默默截断 / 默默不跑的地方先说出来。"""
        if not self.events:
            self._log("parity: 事件表是空的 -> DLL 会报 script: parse failed (no events / bad json)，"
                      "点按钮不会有动作")
        if len(self.events) > MAX_EVENTS:
            self._log("parity: 事件 %d 行 > %d，DLL 只读前 %d 行（试跑会全部跑完）"
                      % (len(self.events), MAX_EVENTS, MAX_EVENTS))

    def _dll_will_run(self) -> tuple[str, str]:
        """按 DLL 的优先级算它下一次会加载哪份：active.txt > 最新 .json。"""
        name = ""
        try:
            # DLL 只读前 159 字节的第一行，并且会无条件补 ".json"
            with open(os.path.join(self.scripts_dir, "active.txt"), "rb") as fh:
                head = fh.read(159)
            name = head.split(b"\r", 1)[0].split(b"\n", 1)[0].decode("utf-8", "replace").strip()
        except OSError:
            name = ""
        # active.txt 指错文件时 DLL 不报错，直接退到「最新 .json」那一档
        if name and os.path.exists(self._script_path(name)):
            return name, "active.txt"

        newest, best = "", 0.0
        if os.path.isdir(self.scripts_dir):
            for fn in os.listdir(self.scripts_dir):
                if not fn.lower().endswith(".json"):
                    continue
                try:
                    mtime = os.path.getmtime(os.path.join(self.scripts_dir, fn))
                except OSError:
                    continue
                if mtime >= best:
                    best, newest = mtime, fn[:-5]
        return newest, "最新 .json"

    def _dll_preview_load(self, path: str) -> tuple[bool, str]:
        """照 ScriptReadFile + JsonParseScript 的判据预演一遍：events 为空就不算加载成功。"""
        try:
            size = os.path.getsize(path)
        except OSError:
            return False, "open failed"
        if size <= 0 or size > 1024 * 1024:
            return False, "bad size"
        try:
            with open(path, "rb") as fh:
                raw = fh.read()
            data = json.loads(raw.decode("utf-8-sig", "replace"))
        except (OSError, ValueError):
            return False, "no events / bad json"
        events = data.get("events") if isinstance(data, dict) else None
        if not isinstance(events, list) or not events:
            return False, "no events / bad json"
        # loop：DLL 取 v>=1 才用，且 clamp 到 99；name 只留 63 个宽字符
        v = data.get("loop")
        loop = int(v) if isinstance(v, (int, float)) and v >= 1 else 1
        loop = min(MAX_LOOP, loop)
        name = data.get("name")
        name = name if isinstance(name, str) else ""
        return True, 'name="%s" events=%d loop=%d' % (name, min(MAX_EVENTS, len(events)), loop)

    def _log_dll_view(self) -> None:
        name, source = self._dll_will_run()
        if not name:
            self._log("dll view: script: no json found, the button does nothing")
            self._log('dll view: 叠加层按钮会显示「无脚本」，点下去没有任何动作（两段点击链已移除）；'
                      'DLL 每次点按钮都会重打上面那行原因')
            return
        path = self._script_path(name)
        if not os.path.exists(path):
            # active.txt 指空了就直接当「没脚本」，不再往下看最新 .json
            self._log('dll view: DLL 要找 "%s"（来源 %s）但文件不存在 -> script: no json found, '
                      "the button does nothing" % (name, source))
            self._log('dll view: 叠加层按钮会显示「无脚本」，点下去没有任何动作（两段点击链已移除）；'
                      'DLL 每次点按钮都会重打上面那行原因')
            return
        ok, detail = self._dll_preview_load(path)
        if ok:
            self._log("dll view: script: loaded %s %s" % (path, detail))
            self._log("dll view: 脚本接管按钮，叠加层不会有常驻提示行")
        else:
            # ScriptReload 走 SetScriptStatus(2, err)：常驻一行「脚本不可用…」，
            # 但胶囊文字不变（只有「根本没脚本文件」才改），点下去不会有任何动作。
            # 按钮是 verbose 调用：同一份被拒收的文件即使没修改，每次点击也会
            # 重打一行 parse failed（DLL 把原因存在 g_scriptErr）。
            self._log('dll view: script: parse failed %s (%s) -> 点按钮不会有动作，'
                      "叠加层常驻「脚本不可用（%s），点按钮不会有动作」"
                      % (path, detail, detail))
        if self.current_name and name != self.current_name:
            # 还没载入过任何脚本（current_name 为空）时不打这条，免得开窗就报空名字
            self._log("警告: DLL 加载的不是当前脚本「%s」——检查 active.txt"
                      % self.current_name)

    def delete_script(self) -> None:
        name = self.script_combo.get()
        if not name:
            return
        if not messagebox.askyesno("删除脚本", "确定删除「%s」？" % name):
            return
        try:
            os.remove(self._script_path(name))
        except OSError as exc:
            messagebox.showerror("删除失败", str(exc))
            return
        self.current_name = ""
        self.refresh_script_list(load_first=True)
        self._status("已删除 %s" % name)

    # -- 执行：与 script_engine.inc 逐行等价 -------------------------------
    def do_run(self) -> None:
        self._start_job(list(self.events), self._int(self.loop_var.get(), 1), 1, "")

    def do_run_selected(self) -> None:
        idx = self._selected_index()
        if idx is None:
            self._status("先选中一行")
            return
        self._start_job([dict(self.events[idx])], 1, idx + 1, "单行")

    def _start_job(self, events: list[dict], loop: int, first_index: int,
                   mode: str) -> None:
        if self.running:
            self._log("script: run ignored, previous run still active")
            return
        if not events:
            messagebox.showinfo("提示", "事件表是空的")
            return
        if not self._ensure_target():
            return
        loop = min(MAX_LOOP, max(1, loop))
        self.stop_evt = threading.Event()
        self.running = True
        # 一次试跑共用一条 CDP 连接。这里不能预先 new 一个未连接的 client：
        # _cdp_ensure 只判断 is not None，一旦预建就会跳过真正 connect，
        # 后面发命令直接报 not connected。
        self._cdp = None
        self._cdp_page = ""
        self._cdp_failed = False
        self._cdp_target = self._cdp_pick_target()   # 没写 page 的行用这张页
        self._bg_msg = bool(self.bg_msg_var.get())           # Tk 变量不能在线程里读
        self._page_origin = None                     # 页面 px → view px 的偏移，每轮重标定
        if self._cdp_target:
            self._log("cdp: 试跑目标页 = %s" % self._cdp_target.split("?", 1)[0])
        self.run_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        threading.Thread(target=self._job_thread,
                         args=(events, loop, first_index, mode,
                               self.name_var.get().strip() or self.current_name or "script",
                               self.target.view, self.target.scale,
                               self.target.w, self.target.h),
                         daemon=True).start()

    def do_stop(self) -> None:
        self.stop_evt.set()
        self._status("正在停止…")

    def _job_thread(self, events, loop, first_index, mode, name,
                    view, scale, view_w, view_h) -> None:
        del mode  # 只有日志前缀用；probe 模式已随门控一起移除
        started = time.time()
        try:
            self.msg_queue.put("target: view %dx%d scale %.2f" % (view_w, view_h, scale))
            self._check_view_size(view_w, view_h)
            self.msg_queue.put('script: run "%s" events=%d loop=%d'
                               % (name, len(events), loop))
            aborted = False
            for round_index in range(1, loop + 1):
                if self.stop_evt.is_set():
                    aborted = True
                    break
                for offset, ev in enumerate(events):
                    if self.stop_evt.is_set():
                        aborted = True
                        break
                    if loop > 1 or len(events) > 1:
                        remark = str(ev.get("remark") or "").strip()
                        line = "—— 第 %d 行 ——" % (first_index + offset)
                        if remark:
                            line += "  %s" % remark
                        self.msg_queue.put(line)
                    last = (round_index == loop and offset == len(events) - 1)
                    self._run_event(ev, view, scale, last)
                if aborted:
                    break
            ms = int((time.time() - started) * 1000)
            if aborted:
                self.msg_queue.put("script: stopped in %dms" % ms)
                self.msg_queue.put("parity: 编辑器能中止，DLL 不能（它跑到最后一行为止）")
            else:
                self.msg_queue.put("script: done in %dms" % ms)
            self.msg_queue.put("__DONE__")
        except Exception as exc:  # 线程里出错要回显，不能静默
            self.msg_queue.put("__ERROR__%s" % exc)
        finally:
            if self._cdp is not None:
                self._cdp.close()
                self._cdp = None

    # -- CDP（选择器行）：与 DLL RunEventActions 的分支、日志逐字对齐 ------
    def _cdp_pick_target(self) -> str:
        """脚本默认打哪张页：最后一张「正经页」（跳过内部隐藏页），
        拿不到列表就交给 CDP 自动挑（空串）。"""
        pages = list_pages_safe()
        if not pages:
            return ""
        return pages[default_page_index(pages)].get("url") or ""

    def _cdp_ensure(self, match: str = "") -> bool:
        """懒连接；本行指定了别的页面就断开换页重连。

        失败一次本轮不重试（DLL CdpEnsure 同策略）。
        """
        if self._cdp is not None and self._cdp_page == match:
            return True
        if self._cdp is not None:      # 换页：先关掉旧连接
            self._cdp.close()
            self._cdp = None
        if self._cdp_failed:
            return False
        client = CdpClient()   # 断线重连：旧实例已被 _cdp_query 清掉
        try:
            url = client.connect(match)
        except (OSError, CdpError, ValueError) as exc:
            client.close()
            self._cdp_failed = True
            self.msg_queue.put(
                "script: cdp unavailable (%s) - selector rows fall back to x/y" % exc)
            return False
        self._cdp = client
        self._cdp_page = match
        # 让页面一直当自己是有焦点的：宿主从前台切走时，页面会进入后台节流，
        # 派发过去的输入要等真正有输入事件才处理（表现为“点了没反应”）
        try:
            client.call("Emulation.setFocusEmulationEnabled", {"enabled": True})
        except (OSError, CdpError, ValueError):
            pass
        self.msg_queue.put('script: cdp connect ok target="%s"' % url)
        return True

    def _cdp_query(self, selector: str) -> dict | None:
        """查元素矩形；None = 查询失败（已打日志），found=0 = 页面里没有。

        查询失败按断线处理：关掉连接，下一行 _cdp_ensure 重连（DLL 的
        CdpCall 失败同样关 socket、CdpEnsure 重解析目标页）。
        """
        assert self._cdp is not None
        try:
            return cdp_query_selector(self._cdp, selector)
        except (OSError, CdpError, KeyError, ValueError) as exc:
            self.msg_queue.put('script: cdp "%s" evaluate failed (%s)'
                               % (selector[:40], exc))
            self._cdp.close()
            self._cdp = None
            return None

    def _cdp_click(self, selector: str, kind: int) -> bool:
        rc = self._cdp_query(selector)
        if rc is None:
            return False
        if not rc.get("found") or rc.get("w", 0) < 0.5 or rc.get("h", 0) < 0.5:
            return False
        try:
            x, y = rc["x"], rc["y"]
            # 先 hover 再点：Vue/React 页面里不少可点元素要 mouseenter 之后才
            # 渲染出来或挂上监听，直接 press 会「点了没反应」
            cdp_hover(self._cdp, x, y)
            rc2 = self._cdp_query(selector)     # hover 后布局可能变，重新定位
            if rc2 is not None and rc2.get("found") \
                    and rc2.get("w", 0) >= 0.5 and rc2.get("h", 0) >= 0.5:
                x, y = rc2["x"], rc2["y"]
            cdp_click_at(self._cdp, x, y, kind)
        except (OSError, CdpError) as exc:
            self.msg_queue.put('script: cdp "%s" input failed (%s)'
                               % (selector[:40], exc))
            return False
        self.msg_queue.put(
            'script: cdp click kind=%d "%s" %.0f,%.0f (page px, trusted input)'
            % (kind, selector[:40], x, y))
        return True

    def _cdp_wheel(self, selector: str, notches: int) -> bool:
        rc = self._cdp_query(selector)
        if rc is None or not rc.get("found") or rc.get("w", 0) < 0.5 or rc.get("h", 0) < 0.5:
            return False
        try:
            self._cdp.call("Input.dispatchMouseEvent",
                           {"type": "mouseWheel", "x": rc["x"], "y": rc["y"],
                            "deltaX": 0, "deltaY": -CDP_DELTA_PER_NOTCH * notches})
        except (OSError, CdpError) as exc:
            self.msg_queue.put('script: cdp "%s" input failed (%s)'
                               % (selector[:40], exc))
            return False
        self.msg_queue.put('script: cdp wheel %+d "%s" %.0f,%.0f (page px)'
                           % (notches, selector[:40], rc["x"], rc["y"]))
        return True

    def show_dom(self) -> None:
        """开一个窗口把 WebView 页面的 DOM 摊成表，用来挑选择器。"""
        try:
            pages = CdpClient().list_pages()
        except (OSError, CdpError, ValueError) as exc:
            messagebox.showerror(
                "页面结构",
                "连不上 CDP（%s:%d）：%s\n\n确认 TT语音 已启动、version.dll 代理已布好端口"
                % (CDP_HOST, CDP_PORT, exc), parent=self.root)
            return
        if not pages:
            messagebox.showwarning("页面结构", "CDP 上还没有页面（%s:%d）"
                                   % (CDP_HOST, CDP_PORT), parent=self.root)
            return
        # 宿主常开多张页（活动页 + 应用内部隐藏页），所以让用户自己挑
        win = DomWindow(self.root, on_fill=self.set_selector_selected,
                        on_refresh=lambda match: self._dom_fill(win, match),
                        pages=pages)
        self._dom_fill(win, win.current_match())

    def _dom_fill(self, win: DomWindow, match: str = "") -> None:
        """连一次 CDP 抓页面结构（连接 + 求值通常几百毫秒，直接同步做）。"""
        if not win.winfo_exists():
            return
        client = CdpClient()
        try:
            url = client.connect(match)
            data = json.loads(client.evaluate(CDP_DOM_JS))
        except (OSError, CdpError, ValueError) as exc:
            messagebox.showerror(
                "页面结构",
                "读不到页面（CDP %s:%d）：%s\n\n确认 TT语音 已启动、version.dll 代理已布好端口"
                % (CDP_HOST, CDP_PORT, exc), parent=self.root)
            return
        finally:
            client.close()
        win.load(data)
        win.lift()
        self._log("dom: %s　%d 个节点" % (url or data.get("url", ""), data.get("n", 0)))
        self._status("页面结构已读取：%d 个节点" % data.get("n", 0))

    def set_selector_selected(self, selector: str, page: str = "") -> None:
        """页面结构窗口里的「填入选中行」：写进当前行的 selector，顺便记下页面。"""
        idx = self._selected_index()
        if idx is None:
            self._status("先在事件表里选中一行，再填选择器")
            return
        self.events[idx]["selector"] = selector
        if page:
            self.events[idx]["page"] = page
        self.dirty = True
        self.render_table()
        self.tree.selection_set(str(idx))
        self._status("第 %d 行：选择器 = %s，页面 = %s"
                     % (idx + 1, selector, self._page_short(page) if page else "默认"))

    def test_selector(self, selector: str, page: str, parent: tk.Toplevel) -> None:
        """事件对话框里的「测试选择器」：连 DLL 同一端口，只查询不点击。"""
        client = CdpClient()
        try:
            client.connect(page)
            rc = cdp_query_selector(client, selector)
            if rc.get("found"):
                messagebox.showinfo(
                    "测试选择器",
                    "找到了：中心 (%.0f, %.0f)，尺寸 %.0f x %.0f（页面 CSS px）"
                    % (rc["x"], rc["y"], rc["w"], rc["h"]), parent=parent)
            else:
                messagebox.showwarning("测试选择器", "页面里没有这个元素", parent=parent)
        except (OSError, CdpError, KeyError, ValueError) as exc:
            messagebox.showerror("测试选择器",
                                 "连不上或查询失败：%s\n（确认宿主已装新版 version.dll 并重启）"
                                 % exc, parent=parent)
        finally:
            client.close()

    def _run_event(self, ev, view, scale, skip_delay) -> None:
        # 每行的 x/y 就是执行坐标（偏移已废弃，DLL 缺 ox/oy 时按 0 加）
        lx = float(ev.get("x", 0) or 0)
        ly = float(ev.get("y", 0) or 0)
        # DLL: static_cast<LONG>(lx * g_scale) —— 截断不是四舍五入，这里保持一致
        px, py = int(lx * scale), int(ly * scale)

        selector = str(ev.get("selector") or "").strip()
        wheel = int(ev.get("wheel") or 0)
        kind = ev.get("click", "left")
        kind_n = CLICK_KINDS.get(kind, 0)
        picked = lx != 0 or ly != 0          # 0,0 / -1,-1 都算「没拾取过」

        # 与 DLL RunEventActions 同一分支：有选择器先走 CDP，失败回退坐标。
        # 本行写了 page 就按那张页解析（换页会重连），没写就用脚本默认页。
        page = str(ev.get("page") or "").strip() or self._cdp_target
        cdp = bool(selector) and self._cdp_ensure(page)

        if wheel:
            if selector and cdp:
                if not self._cdp_wheel(selector, wheel):
                    self._post_wheel_legacy(view, px, py, wheel)
            else:
                self._post_wheel_legacy(view, px, py, wheel)

        # 「后台消息」模式：某些页面的交互元素只认落在窗口上的鼠标消息——CDP
        # 派发过去 DOM 的 click 会触发，但业务不响应。这时把选择器解析出来的
        # 位置换算成 view 坐标，往 view 投 WM_MOUSEMOVE/DOWN/UP（不置前、不动
        # 光标）。页面 CSS px 与 view 逻辑 px 同单位，只差一个 WebView 面板的
        # 偏移，用「既有选择器又有坐标」的行标定一次即可（本轮试跑内有效）。
        if self._bg_msg and selector:
            rc = self._cdp_query(selector) if cdp else None
            if rc is not None and rc.get("found"):
                if picked:
                    self._page_origin = (lx - rc["x"], ly - rc["y"])   # 标定偏移
                    self._post_click_msg(view, px, py, kind, kind_n)
                elif self._page_origin is not None:
                    ex = int((rc["x"] + self._page_origin[0]) * scale)
                    ey = int((rc["y"] + self._page_origin[1]) * scale)
                    self._post_click_msg(view, ex, ey, kind, kind_n)
                else:
                    self.msg_queue.put(
                        'script: cdp "%s" bg message needs one picked x/y to '
                        'calibrate, falls back to cdp click' % selector[:40])
                    self._cdp_click(selector, kind_n)
            elif picked:                       # 页面里没这个元素，按坐标点
                self._post_click_msg(view, px, py, kind, kind_n)
            else:
                self.msg_queue.put(
                    'script: cdp "%s" not found and no x/y fallback, skipped'
                    % selector[:40])
        elif not selector:
            self._post_click_legacy(view, px, py, kind, kind_n)
        elif cdp and self._cdp_click(selector, kind_n):
            pass
        elif lx == 0 and ly == 0:
            self.msg_queue.put(
                'script: cdp "%s" not found and no x/y fallback, skipped'
                % selector[:40])
        else:
            if cdp:
                self.msg_queue.put(
                    'script: cdp "%s" missing, click falls back to x/y'
                    % selector[:40])
            self._post_click_legacy(view, px, py, kind, kind_n)

        delay = max(0, int(ev.get("delay_ms") or 0))
        if delay and not skip_delay:
            self.stop_evt.wait(delay / 1000.0)

    def _post_wheel_legacy(self, view: int, px: int, py: int, wheel: int) -> None:
        ok, info = post_wheel(view, px, py, wheel)
        if ok:
            self.msg_queue.put("script: wheel %+d at %s (real input)" % (wheel, info))
        else:
            self.msg_queue.put("script: wheel skipped, covered by %s"
                               "（把 TTVoice 窗口露出来再跑）" % info)

    def _post_click_legacy(self, view: int, px: int, py: int, kind: str,
                           kind_n: int) -> None:
        ok, info = post_click(view, px, py, kind)
        if ok:
            self.msg_queue.put("script: click kind=%d %s (real input)"
                               % (kind_n, info))
        else:
            self.msg_queue.put("script: click skipped, covered by %s"
                               "（把 TTVoice 窗口露出来再跑）" % info)

    def _post_click_msg(self, view: int, px: int, py: int, kind: str,
                        kind_n: int) -> None:
        ok, info = post_click_message(view, px, py, kind)
        if ok:
            self.msg_queue.put("script: click kind=%d %s (bg message)"
                               % (kind_n, info))
        else:
            self.msg_queue.put("script: click skipped, view gone")

    def _check_view_size(self, w: int, h: int) -> None:
        recorded = self.script_view_size
        if recorded and (recorded[0] != w or recorded[1] != h):
            self.msg_queue.put("parity: 脚本是按 view %dx%d 拾的坐标，现在 %dx%d，位置会偏"
                               % (recorded[0], recorded[1], w, h))

    # -- 日志窗格 / 跟随 DLL ------------------------------------------------
    def _on_follow_toggle(self) -> None:
        # 不能在后台线程读 Tk 变量（会撞上「main thread is not in main loop」），
        # 所以在这里把开关存成一个普通字段给轮询线程用。
        self.follow_on = bool(self.follow_var.get())
        if not self.follow_on:
            self._log("follow: 已停止拉 DLL 日志")

    def _clear_log(self) -> None:
        self.log_box.config(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.config(state="disabled")

    @staticmethod
    def _now() -> str:
        t = time.time()
        return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)

    def _log(self, text: str) -> None:
        text = str(text)
        stamp = "" if text.startswith("[DLL]") else self._now() + "  "
        tags = []
        if text.startswith("[DLL]"):
            tags.append("dll")
        if any(m in text for m in WARN_MARKS):
            tags = ["warn"]
        self.log_box.config(state="normal")
        self.log_box.insert("end", stamp + text + "\n", tuple(tags))
        lines = int(self.log_box.index("end-1c").split(".")[0])
        if lines > 500:                     # 只留最近 400 行
            self.log_box.delete("1.0", "%d.0" % (lines - 400))
        self.log_box.config(state="disabled")
        self.log_box.see("end")

    def _follow_thread(self) -> None:
        """把 DLL 写到 %TEMP% 的日志行拉进窗格（前缀 [DLL]）。"""
        offset = 0
        first = True
        while True:
            time.sleep(0.4)
            if not self.follow_on:
                first = True
                continue
            try:
                size = os.path.getsize(DLL_LOG)
            except OSError:
                continue
            if size < offset:
                offset, first = 0, True     # 日志被清掉了
            if first:
                start = max(0, size - 4000)
                try:
                    with open(DLL_LOG, "rb") as fh:
                        fh.seek(start)
                        if start:
                            fh.readline()    # 丢掉可能被截断的第一行
                        offset = fh.tell()
                except OSError:
                    continue
                first = False
                self.msg_queue.put("[DLL] 开始跟随 %s（只从最近几行起）" % DLL_LOG)
                continue
            try:
                with open(DLL_LOG, "rb") as fh:
                    fh.seek(offset)
                    data = fh.read()
            except OSError:
                continue
            if not data:
                continue
            cut = data.rfind(b"\n")
            if cut < 0:
                continue                    # 还不满一行，下次再读
            offset += cut + 1
            for raw in data[:cut].split(b"\n"):
                line = raw.decode("utf-8", "replace").strip()
                if line:
                    self.msg_queue.put("[DLL] %s" % line)

    def _end_job(self) -> None:
        self.running = False
        self.run_btn.config(state="normal")
        self.stop_btn.config(state="disabled")

    def _poll_messages(self) -> None:
        try:
            while True:
                msg = self.msg_queue.get_nowait()
                if msg == "__DONE__":
                    self._end_job()
                    self._status("执行结束")
                elif isinstance(msg, str) and msg.startswith("__ERROR__"):
                    self._end_job()
                    self._log(msg)
                    self._status("执行出错：%s" % msg[len("__ERROR__"):])
                else:
                    self._log(msg)
                    self._status(msg)
        except queue.Empty:
            pass
        self.root.after(100, self._poll_messages)

    def _status(self, text: str) -> None:
        self.status_var.set(str(text))


def main() -> None:
    enable_dpi_awareness()
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
