"""pplugin 点击脚本编辑器（极简版）。

独立窗口，做三件事：
  1. 拾取坐标——点一下目标位置即记录（这次点击会被吞掉，不会触发应用），
     坐标按 FLUTTERVIEW 相对逻辑 px（96 dpi）保存；
  2. 编辑事件表（坐标 / 点击类型 / 间隔 / 滚轮 / 备注）。第一行是「第一坐标」
     （绝对基准），其余行按相对它的「偏移坐标」编辑——改第一坐标，其余行
     自动跟着平移；
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
        {"x": 27, "y": 113, "ox": 0, "oy": 0, "click": "left", "delay_ms": 600,
         "wheel": 0, "remark": ""},
        {"x": 27, "y": 113, "ox": 1546, "oy": -26, "click": "left", "delay_ms": 0,
         "wheel": 0, "remark": ""}
      ]
    }

字段含义：
  x, y          第一坐标（基准），每一行都写同样的值；FLUTTERVIEW 客户区相对
                逻辑 px（96 dpi），物理 px = 逻辑 * DPI/96
  ox, oy        本行相对第一坐标的偏移（每一行都可非 0，含第一行；默认 0,0）
  执行坐标      **真正执行的是 (x + ox, y + oy)**：DLL 解析完就把偏移折算进
                x/y，编辑器试跑同样按这个点执行。改第一坐标（x/y）即整条链
                平移，ox/oy 不变。老脚本没有 ox/oy，折算时加 0，按其绝对
                x/y 执行；编辑器保存时会补齐 ox/oy 并把 x/y 统一写成基准
  click         left | double | right
  selector      可选 CSS 选择器。填了本行先走 CDP：Runtime.evaluate 定位元素
                （getBoundingClientRect，页面 CSS px），Input.dispatchMouseEvent
                在元素中心派发鼠标事件——事件走浏览器真实输入管线
                （isTrusted=true），不需要目标窗口在前台、不怕被遮挡。CDP
                不可用或页面里没有该元素时，回退到本行坐标的 SendInput 点击
                （x/y 都是 0 视为「没拾取过」，跳过并打日志）。端口由
                version.dll 代理在宿主启动时布下（9223），编辑器试跑/测试
                选择器连的是同一个端口
  target_url    可选，脚本级。宿主里开了多个 WebView 页面时按 URL 子串挑目
                标页（大小写不敏感）；空 = 唯一页直接用，多页取最后一条
                （新页追加在列表尾部）
  hotkey        F1..F24 / 单个字母或数字；空 = 不注册（避免抢应用自己的键）
  loop          整份脚本重复执行的轮数
  delay_ms      本行执行完之后的等待（下一行开始前）
  wheel         滚轮格数：正=向上，负=向下，0=不滚；有 selector 时滚在元素上
                （CDP deltaY = -100 * 格数，与 DLL 一致）
  remark        备注，随便写（原 note 键已更名）；DLL 用 JsonSkip 跳过，只给人和编辑器看
  view_size     编辑器额外写的 [w,h]（保存时的 view 客户区），DLL 不认这个键，只用来提醒坐标基准变了

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


def cdp_mouse(client: CdpClient, mtype: str, x: float, y: float,
              button: str | None = None, click_count: int = 0) -> None:
    params: dict = {"type": mtype, "x": x, "y": y}
    if button:
        params["button"] = button
        params["clickCount"] = click_count
    client.call("Input.dispatchMouseEvent", params)


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
        return user32.CallNextHookEx(None, n_code, w_param, l_param)


# --- 事件编辑对话框 -------------------------------------------------------
class EventDialog(tk.Toplevel):
    FIELDS = ("x", "y", "ox", "oy", "click", "delay_ms", "wheel", "selector",
              "remark")

    def __init__(self, master, event: dict, on_pick, on_cancel=None,
                 base: tuple[float, float] = (0.0, 0.0),
                 on_test_selector=None):
        super().__init__(master)
        self.title("事件")
        self.resizable(False, False)
        self.result = None
        self._on_pick = on_pick
        self._on_cancel = on_cancel
        self._base = base
        self._on_test_selector = on_test_selector

        # X/Y = 第一坐标（基准，各行相同，改了整条链平移）；
        # DX/DY = 本行相对第一坐标的偏移（每一行都可改，包括第一行）。
        # 本行执行点 = (x + ox, y + oy)。
        self.x_var = tk.StringVar(value=self._fmt(event.get("x", 0)))
        self.y_var = tk.StringVar(value=self._fmt(event.get("y", 0)))
        self.dx_var = tk.StringVar(value=self._fmt(event.get("ox", 0)))
        self.dy_var = tk.StringVar(value=self._fmt(event.get("oy", 0)))
        self.click_var = tk.StringVar(value=CLICK_LABELS.get(event.get("click", "left"), "左键单击"))
        self.delay_var = tk.StringVar(value=str(event.get("delay_ms", 0)))
        self.wheel_var = tk.StringVar(value=str(event.get("wheel", 0)))
        self.sel_var = tk.StringVar(value=str(event.get("selector") or "").strip())
        self.note_var = tk.StringVar(value=event.get("remark", "") or "")

        body = ttk.Frame(self, padding=10)
        body.grid(row=0, column=0, sticky="nsew")

        grid = [
            (0, "X（第一坐标，逻辑 px）", ttk.Entry(body, textvariable=self.x_var, width=12)),
            (1, "Y（第一坐标，逻辑 px）", ttk.Entry(body, textvariable=self.y_var, width=12)),
            (2, "偏移 DX（本行，相对第一坐标）", ttk.Entry(body, textvariable=self.dx_var, width=12)),
            (3, "偏移 DY（本行，相对第一坐标）", ttk.Entry(body, textvariable=self.dy_var, width=12)),
            (4, "点击事件", ttk.Combobox(body, textvariable=self.click_var, width=12,
                                         state="readonly", values=list(CLICK_LABELS.values()))),
            (5, "间隔时间 ms（点完后等）", ttk.Entry(body, textvariable=self.delay_var, width=12)),
            (6, "滚轮格数（负=向下）", ttk.Entry(body, textvariable=self.wheel_var, width=12)),
            (7, "CSS 选择器（可选，CDP 点击）", ttk.Entry(body, textvariable=self.sel_var, width=32)),
            (8, "备注（DLL 不看）", ttk.Entry(body, textvariable=self.note_var, width=32)),
        ]
        for row, label, widget in grid:
            ttk.Label(body, text=label).grid(row=row, column=0, sticky="w", pady=3, padx=(0, 8))
            widget.grid(row=row, column=1, sticky="w", pady=3)

        # 拾取坐标 = 取第一坐标（基准）；拾取偏移 = 取本行落点换算成偏移
        self.pick_xy_btn = ttk.Button(body, text="拾取坐标\n（第一坐标）", width=10,
                                      command=self._pick_xy)
        self.pick_xy_btn.grid(row=0, column=2, rowspan=2, sticky="ns", padx=(8, 0))
        self.pick_off_btn = ttk.Button(body, text="拾取偏移\n（本行点击点）", width=10,
                                       command=self._pick_off)
        self.pick_off_btn.grid(row=2, column=2, rowspan=2, sticky="ns", padx=(8, 0))

        # 有选择器就能现场验证（连 DLL 同一个 CDP 端口查一次元素矩形）
        if self._on_test_selector is not None:
            self.test_btn = ttk.Button(body, text="测试选择器", width=10,
                                       command=self._test_selector)
            self.test_btn.grid(row=7, column=2, sticky="ns", padx=(8, 0))

        # 实时显示本行真正的执行点，基准/偏移没配对一眼就能看出来
        self.exec_var = tk.StringVar(value="")
        ttk.Label(body, textvariable=self.exec_var, foreground="#B06",
                  font=("Microsoft YaHei UI", 9, "bold")).grid(
            row=9, column=0, columnspan=3, sticky="w")
        for v in (self.x_var, self.y_var, self.dx_var, self.dy_var):
            v.trace_add("write", lambda *_: self._update_exec())
        self._update_exec()

        ttk.Label(body, text="本行执行点 = 第一坐标 + 偏移；填了选择器则按元素点击（坐标作后备）",
                  foreground="#777").grid(row=10, column=0, columnspan=3, sticky="w")

        buttons = ttk.Frame(body)
        buttons.grid(row=11, column=0, columnspan=3, sticky="e", pady=(10, 0))
        ttk.Button(buttons, text="确定", command=self._ok).pack(side="left", padx=4)
        ttk.Button(buttons, text="取消", command=self.destroy).pack(side="left")

        self.transient(master)
        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self.destroy)

    @staticmethod
    def _fmt(v: float) -> str:
        v = round(float(v), 1)
        return str(int(v)) if v.is_integer() else str(v)

    def _update_exec(self):
        """实时显示 第一坐标+偏移 的结果，执行点一眼可查。"""
        try:
            ex = float(self.x_var.get()) + float(self.dx_var.get())
            ey = float(self.y_var.get()) + float(self.dy_var.get())
        except ValueError:
            self.exec_var.set("本行执行点 = 第一坐标 + 偏移 = （有字段不是数字）")
            return
        self.exec_var.set("本行执行点（脚本真正点这里）= (%s, %s)"
                          % (self._fmt(ex), self._fmt(ey)))

    def _pick_xy(self):
        def fill(lx, ly):
            if not self.winfo_exists():
                return
            self.x_var.set(self._fmt(lx))
            self.y_var.set(self._fmt(ly))
        self._on_pick(fill)

    def _pick_off(self):
        def fill(lx, ly):
            if not self.winfo_exists():
                return
            self.dx_var.set(self._fmt(lx - self._base[0]))
            self.dy_var.set(self._fmt(ly - self._base[1]))
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

    def _test_selector(self):
        """现场验证选择器（连 DLL 同一个 CDP 端口，只查询不点击）。"""
        sel = self.sel_var.get().strip()
        if not sel:
            messagebox.showinfo("测试选择器", "先填一个 CSS 选择器", parent=self)
            return
        if self._on_test_selector is not None:
            self._on_test_selector(sel, self)

    def _ok(self):
        try:
            event = {
                "x": float(self.x_var.get()),
                "y": float(self.y_var.get()),
                "ox": float(self.dx_var.get()),
                "oy": float(self.dy_var.get()),
                "click": LABEL_CLICKS.get(self.click_var.get(), "left"),
                "delay_ms": int(float(self.delay_var.get() or 0)),
                "wheel": int(float(self.wheel_var.get() or 0)),
                "selector": self.sel_var.get().strip(),
                "remark": self.note_var.get().strip(),
            }
        except ValueError:
            messagebox.showerror("格式错误", "坐标/偏移/间隔/滚轮必须是数字", parent=self)
            return
        for key in ("x", "y", "ox", "oy"):
            if float(event[key]).is_integer():
                event[key] = int(event[key])
        self.result = event
        self.destroy()


# --- 主窗口 ---------------------------------------------------------------
DEFAULT_EVENTS = [
    {"x": 27, "y": 113, "ox": 0, "oy": 0, "click": "left", "delay_ms": 600,
     "wheel": 0, "selector": "", "remark": ""},
    {"x": 27, "y": 113, "ox": 1546, "oy": -26, "click": "left", "delay_ms": 0,
     "wheel": 0, "selector": "", "remark": ""},
]


class App:
    COLUMNS = (
        ("idx", "序号", 50, "center"),
        ("pos", "坐标", 120, "center"),
        ("off", "偏移", 110, "center"),
        ("click", "点击", 90, "center"),
        ("delay", "间隔ms", 80, "center"),
        ("wheel", "滚轮", 70, "center"),
        ("sel", "选择器", 120, "w"),
        ("note", "备注", 130, "w"),
    )

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("pplugin 点击脚本编辑器")
        # Tk 的字体按 DPI 放大（150% 屏 scaling=2.0），几何/列宽也要同步放大，
        # 否则文字放大了而窗口没放大，布局会挤成一团。
        self.scale = self._ui_scale()
        self.root.geometry("%dx%d" % (int(1000 * self.scale), int(800 * self.scale)))
        self.root.minsize(int(820 * self.scale), int(600 * self.scale))

        self.scripts_dir = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "scripts")
        os.makedirs(self.scripts_dir, exist_ok=True)

        self.target = Target()
        self.events: list[dict] = [dict(e) for e in DEFAULT_EVENTS]
        self.current_name = ""
        self.dirty = True

        self.pick_queue: queue.Queue | None = None
        self.picker: ClickPicker | None = None
        self.pick_cb = None

        self.msg_queue: queue.Queue = queue.Queue()
        self.stop_evt = threading.Event()
        self.running = False
        self.follow_var = tk.BooleanVar(value=False)   # 只给勾选项用（Tk 线程）
        self.follow_on = False                         # 后台线程只读这个普通变量
        self.script_view_size: tuple[int, int] | None = None

        self.name_var = tk.StringVar(value="活动")
        self.hotkey_var = tk.StringVar(value="")
        self.loop_var = tk.StringVar(value="1")
        self.target_var = tk.StringVar(value="")     # target_url：多个页面时选页
        self.status_var = tk.StringVar(value="")

        # 一次试跑共用一条 CDP 连接；失败一次后本轮不再重试（与 DLL 一致）
        self._cdp: CdpClient | None = None
        self._cdp_failed = False
        self._cdp_target = ""

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
    def _build(self) -> None:
        outer = ttk.Frame(self.root, padding=8)
        outer.pack(fill="both", expand=True)

        top = ttk.LabelFrame(outer, text="脚本", padding=6)
        top.pack(fill="x")

        ttk.Label(top, text="文件").pack(side="left")
        self.script_combo = ttk.Combobox(top, width=16, state="readonly")
        self.script_combo.pack(side="left", padx=4)
        self.script_combo.bind("<<ComboboxSelected>>", self._on_script_selected)
        ttk.Button(top, text="刷新", width=6, command=self.refresh_script_list).pack(side="left")
        ttk.Button(top, text="新建", width=6, command=self.new_script).pack(side="left", padx=(4, 0))
        ttk.Button(top, text="保存", width=6, command=self.save_script).pack(side="left", padx=(4, 0))
        ttk.Button(top, text="另存为", width=8, command=self.save_script_as).pack(side="left", padx=(4, 0))
        ttk.Button(top, text="删除", width=6, command=self.delete_script).pack(side="left", padx=(4, 0))

        meta = ttk.Frame(outer, padding=(0, 6))
        meta.pack(fill="x")
        ttk.Label(meta, text="名称").pack(side="left")
        ttk.Entry(meta, textvariable=self.name_var, width=16).pack(side="left", padx=(4, 12))
        ttk.Label(meta, text="启动热键").pack(side="left")
        ttk.Entry(meta, textvariable=self.hotkey_var, width=8).pack(side="left", padx=(4, 12))
        ttk.Label(meta, text="循环次数").pack(side="left")
        ttk.Entry(meta, textvariable=self.loop_var, width=6).pack(side="left", padx=(4, 12))
        ttk.Label(meta, text="目标页URL").pack(side="left")
        ttk.Entry(meta, textvariable=self.target_var, width=24).pack(side="left", padx=(4, 12))
        ttk.Label(meta, text="（热键：空 = 不注册；目标页 = URL 子串，空 = 自动选）",
                  foreground="#777").pack(side="left")

        tools = ttk.Frame(outer)
        tools.pack(fill="x", pady=(0, 6))
        ttk.Button(tools, text="添加事件", command=self.add_manual).pack(side="left")
        ttk.Button(tools, text="修改", command=self.edit_selected).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="删除行", command=self.delete_selected).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="上移", width=6, command=lambda: self.move_row(-1)).pack(side="left", padx=(4, 0))
        ttk.Button(tools, text="下移", width=6, command=lambda: self.move_row(1)).pack(side="left", padx=(4, 0))

        # 先占住底部条，再放会 expand 的表格；否则表格会把底部挤掉。
        bottom = self.bottom = ttk.Frame(outer, padding=(0, 6))
        bottom.pack(side="bottom", fill="x")
        self.run_btn = ttk.Button(bottom, text="试跑", command=self.do_run)
        self.run_btn.pack(side="left")
        ttk.Button(bottom, text="跑选中行", command=self.do_run_selected).pack(
            side="left", padx=(4, 0))
        self.stop_btn = ttk.Button(bottom, text="停止", command=self.do_stop, state="disabled")
        self.stop_btn.pack(side="left", padx=(4, 12))
        ttk.Button(bottom, text="刷新目标", width=10,
                   command=lambda: self.refresh_target()).pack(side="left")
        ttk.Checkbutton(bottom, text="跟随 DLL 日志", variable=self.follow_var,
                        command=self._on_follow_toggle).pack(side="left", padx=(12, 0))
        ttk.Button(bottom, text="清日志", width=7, command=self._clear_log).pack(
            side="left", padx=(4, 0))
        ttk.Label(bottom, textvariable=self.status_var, foreground="#555").pack(
            side="left", padx=12, fill="x", expand=True)

        # 执行日志：跑脚本时逐行输出 DLL 同字样的痕迹，[DLL] 前缀的是
        # DLL 自己写的行，两边可以直接逐行比对。
        logf = self.logf = ttk.LabelFrame(outer, text="执行日志（字样与 DLL 一致，[DLL] 是从 %TEMP% 拉的）",
                                          padding=4)
        logf.pack(side="bottom", fill="x")
        self.log_box = tk.Text(logf, height=9, wrap="none", state="disabled",
                                font=("Consolas", 9), relief="flat",
                                background="#1E1F26", foreground="#E3E7EE")
        log_scroll = ttk.Scrollbar(logf, orient="vertical", command=self.log_box.yview)
        self.log_box.configure(yscrollcommand=log_scroll.set)
        self.log_box.pack(side="left", fill="both", expand=True)
        log_scroll.pack(side="right", fill="y")
        self.log_box.tag_config("warn", foreground="#FFB454")
        self.log_box.tag_config("dll", foreground="#8FD3A0")

        table = self.table_frame = ttk.Frame(outer)
        table.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(table, columns=[c[0] for c in self.COLUMNS], show="headings",
                                 selectmode="browse")
        for key, title, width, anchor in self.COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=int(width * self.scale), anchor=anchor,
                             stretch=(key == "note"))
        scroll = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.tree.bind("<Double-1>", lambda _e: self.edit_selected())
        self.tree.bind("<Delete>", lambda _e: self.delete_selected())

    # -- 目标窗口 ----------------------------------------------------------
    def refresh_target(self, quiet: bool = False) -> bool:
        self.target = find_target()
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

    # -- 表格 --------------------------------------------------------------
    def render_table(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, ev in enumerate(self.events, 1):
            off = "%s, %s" % (self._num(ev.get("ox", 0)), self._num(ev.get("oy", 0)))
            sel = str(ev.get("selector") or "").strip()
            self.tree.insert("", "end", iid=str(i - 1), values=(
                i,
                "%s, %s" % (self._exec(ev, "x", "ox"), self._exec(ev, "y", "oy")),
                off,
                CLICK_LABELS.get(ev.get("click", "left"), "左键单击"),
                ev.get("delay_ms", 0),
                ev.get("wheel", 0),
                sel if len(sel) <= 24 else sel[:23] + "…",
                ev.get("remark", "") or "",
            ))

    @staticmethod
    def _exec(ev, key, off_key):
        # 执行坐标 = 第一坐标 + 偏移（与 DLL 的 x+ox 折算一致）
        try:
            v = float(ev.get(key, 0) or 0) + float(ev.get(off_key, 0) or 0)
        except (TypeError, ValueError):
            return "?"
        return App._num(round(v, 1))

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

    def _base(self) -> tuple[float, float]:
        """第一坐标（第一行）的绝对值，没有行时为 (0, 0)。"""
        if not self.events:
            return (0.0, 0.0)
        return (float(self.events[0].get("x", 0) or 0),
                float(self.events[0].get("y", 0) or 0))

    def _normalize_events(self) -> None:
        """统一成新格式：x/y = 第一坐标（各行相同），ox/oy = 偏移，note→remark。"""
        if not self.events:
            return
        bx = float(self.events[0].get("x", 0) or 0)
        by = float(self.events[0].get("y", 0) or 0)
        for i, ev in enumerate(self.events):
            if "remark" not in ev:
                ev["remark"] = str(ev.pop("note", "") or "")
            if "ox" not in ev or "oy" not in ev:
                if i == 0:
                    ev["ox"] = 0
                    ev["oy"] = 0
                # 老格式：x/y 是绝对执行坐标，换算成相对基准的偏移
                ev["ox"] = self._shift(ev.get("x", 0), -bx)
                ev["oy"] = self._shift(ev.get("y", 0), -by)
            ev["x"] = self._shift(bx, 0)
            ev["y"] = self._shift(by, 0)

    def add_manual(self) -> None:
        bx, by = self._base()
        ev = {"x": bx, "y": by, "ox": 0, "oy": 0, "click": "left",
              "delay_ms": 0, "wheel": 0, "selector": "", "remark": ""}
        dialog = EventDialog(self.root, ev, self.start_pick, self._end_pick,
                             base=self._base(),
                             on_test_selector=self.test_selector)
        self.root.wait_window(dialog)
        if dialog.result:
            self._add_event(dialog.result)

    def edit_selected(self) -> None:
        idx = self._selected_index()
        if idx is None:
            self._status("先选中一行")
            return
        ev = dict(self.events[idx])
        old_base = self._base()
        dialog = EventDialog(self.root, ev, self.start_pick, self._end_pick,
                             base=old_base, on_test_selector=self.test_selector)
        self.root.wait_window(dialog)
        if dialog.result:
            self.events[idx] = dialog.result
            # 对话框里改了第一坐标 = 整条链平移：各行 x/y 统一成新基准
            if (float(dialog.result["x"]), float(dialog.result["y"])) != old_base:
                for other in self.events:
                    other["x"] = dialog.result["x"]
                    other["y"] = dialog.result["y"]
                self._status("第一坐标已更新为 (%s,%s)，整条链随之平移"
                             % (self._num(dialog.result["x"]),
                                self._num(dialog.result["y"])))
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
        self._status("拾取中：请在 TT语音 界面上点一下目标位置…")
        self.root.after(80, self._poll_pick)

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

        px, py, lx, ly = rest
        self._status("拾取到 物理(%s,%s) → 逻辑(%s,%s)" % (px, py, lx, ly))
        cb = self.pick_cb
        self._end_pick()
        if cb:
            cb(lx, ly)

    def _end_pick(self) -> None:
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
        self.target_var.set(str(data.get("target_url") or ""))
        self.events = [dict(e) for e in data.get("events", [])]
        self._normalize_events()
        size = data.get("view_size") or []
        self.script_view_size = (int(size[0]), int(size[1])) if len(size) == 2 else None
        self.dirty = False
        self.script_combo.set(name)
        self.render_table()
        self._status("已载入 %s（%d 行）" % (path, len(self.events)))

    def _collect(self) -> dict:
        w, h = self.target.w, self.target.h
        bx = float(self.events[0].get("x", 0) or 0) if self.events else 0.0
        by = float(self.events[0].get("y", 0) or 0) if self.events else 0.0
        events = []
        for i, ev in enumerate(self.events):
            item = dict(ev)
            item.pop("text", None)  # 粘贴功能已移除，老脚本里的 text 一并清掉
            # 门控已整体移除，老脚本里的这两个键一并清掉（DLL 当未知键跳过）
            item.pop("wait_px", None)
            item.pop("wait_max_ms", None)
            # 老脚本没有 remark 键，保存时补齐，DLL 会 JsonSkip 掉
            item["remark"] = str(item.get("remark") or "").strip()
            # selector 同理补齐（空 = 本行走坐标点击），DLL 认这个键
            item["selector"] = str(item.get("selector") or "").strip()
            # x/y 每行统一写第一坐标；ox/oy 缺失时（老格式：x/y 是绝对执行坐
            # 标）按与基准的差值补齐。DLL 执行 (x+ox, y+oy)。
            if i == 0:
                item.setdefault("ox", 0)
                item.setdefault("oy", 0)
            elif "ox" not in item or "oy" not in item:
                item["ox"] = self._shift(item.get("x", 0), -bx)
                item["oy"] = self._shift(item.get("y", 0), -by)
            item["x"] = self._shift(bx, 0)
            item["y"] = self._shift(by, 0)
            events.append(item)
        data = {
            "version": 1,
            "name": self.name_var.get().strip() or self.current_name or "script",
            "hotkey": self.hotkey_var.get().strip(),
            "loop": min(MAX_LOOP, max(1, self._int(self.loop_var.get(), 1))),
            "events": events,
        }
        # target_url：多个页面时按 URL 子串挑目标页（空 = 自动挑，不写进 JSON）
        target = self.target_var.get().strip()
        if target:
            data["target_url"] = target
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
        self.target_var.set("")
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
        self._status("已保存 %s" % path)
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
        # 一次试跑共用一条 CDP 连接；target_url 在 Tk 线程里读，线程里只用副本
        self._cdp = CdpClient()
        self._cdp_failed = False
        self._cdp_target = self.target_var.get().strip()
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
                        self.msg_queue.put("—— 第 %d 行 ——" % (first_index + offset))
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
    def _cdp_ensure(self) -> bool:
        """懒连接；失败一次本轮不重试（DLL CdpEnsure 同策略）。"""
        if self._cdp is not None:
            return True
        if self._cdp_failed:
            return False
        client = CdpClient()   # 断线重连：旧实例已被 _cdp_query 清掉
        try:
            url = client.connect(self._cdp_target)
        except (OSError, CdpError, ValueError) as exc:
            client.close()
            self._cdp_failed = True
            self.msg_queue.put(
                "script: cdp unavailable (%s) - selector rows fall back to x/y" % exc)
            return False
        self._cdp = client
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
            cdp_click_at(self._cdp, rc["x"], rc["y"], kind)
        except (OSError, CdpError) as exc:
            self.msg_queue.put('script: cdp "%s" input failed (%s)'
                               % (selector[:40], exc))
            return False
        self.msg_queue.put(
            'script: cdp click kind=%d "%s" %.0f,%.0f (page px, trusted input)'
            % (kind, selector[:40], rc["x"], rc["y"]))
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

    def test_selector(self, selector: str, parent: tk.Toplevel) -> None:
        """事件对话框里的「测试选择器」：连 DLL 同一端口，只查询不点击。"""
        client = CdpClient()
        try:
            client.connect(self.target_var.get().strip())
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
        # DLL：解析完把偏移折算进 x/y，执行点 = (x+ox, y+oy)。这里同样折算。
        lx = float(ev.get("x", 0) or 0) + float(ev.get("ox", 0) or 0)
        ly = float(ev.get("y", 0) or 0) + float(ev.get("oy", 0) or 0)
        # DLL: static_cast<LONG>(lx * g_scale) —— 截断不是四舍五入，这里保持一致
        px, py = int(lx * scale), int(ly * scale)

        selector = str(ev.get("selector") or "").strip()
        wheel = int(ev.get("wheel") or 0)
        kind = ev.get("click", "left")
        kind_n = CLICK_KINDS.get(kind, 0)
        # 与 DLL RunEventActions 同一分支：有选择器先走 CDP，失败回退坐标
        cdp = bool(selector) and self._cdp_ensure()

        if wheel:
            if selector and cdp:
                if not self._cdp_wheel(selector, wheel):
                    self._post_wheel_legacy(view, px, py, wheel)
            else:
                self._post_wheel_legacy(view, px, py, wheel)

        if not selector:
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

    def _check_view_size(self, w: int, h: int) -> None:
        recorded = self.script_view_size
        if recorded and (recorded[0] != w or recorded[1] != h):
            self.msg_queue.put("parity: 脚本是按 view %dx%d 拾的坐标，现在 %dx%d，位量会偏"
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
