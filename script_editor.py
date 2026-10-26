"""pplugin 脚本事件编辑器。

做三件事：
  1. 拾取坐标——点一下目标位置即记录（这次点击会被吞掉，不会触发应用），
     坐标按 FLUTTERVIEW 相对逻辑 px（96 dpi）保存；
  2. 编辑事件表：鼠标 / 延迟 / 找图（暂未实现）/ 输入文本 / 按键与组合键；
  3. 保存成 JSON，并通过 ctypes 加载 script-engine.dll，逐事件调用执行（试跑）。

与 DLL 的关系：
  script-engine.dll 已不再是插件，而是一个纯执行库，导出：
    PP_Mouse(HWND, x, y, button, action)
    PP_Delay(ms)
    PP_FindImage(HWND, image, threshold, timeout_ms)   # 未实现，返回 -4
    PP_Version()
  编辑器自己控制事件循环，事件逐条交给 DLL 执行；找图事件返回 -4 时记日志并跳过。
  输入/按键事件不经过 DLL：编辑器直接 PostMessageW(WM_CHAR) 发文本、
  PostMessageW(WM_KEYDOWN/UP) 发按键，不抢前台、不动真实键盘；组合键会先把
  修饰键写进目标线程的键盘状态表，目标查 GetKeyState 时也能看到「按住」。

脚本目录：%APPDATA%\\TTVoice\\scripts\\<名称>.json
运行：python script_editor.py

JSON 格式（version 2）：

    {
      "version": 2,
      "name": "活动",
      "loop": 1,
      "events": [
        {"type": "mouse", "action": "click", "button": "left",
         "x": 27, "y": 113, "remark": ""},
        {"type": "delay", "ms": 500, "remark": ""},
        {"type": "findImage", "image": "btn.png", "threshold": 0.9,
         "timeout_ms": 3000, "x": 0, "y": 0, "remark": ""},
        {"type": "input", "text": "你好 hello", "remark": ""},
        {"type": "key", "key": "Ctrl+Shift+A", "action": "press", "remark": ""}
      ]
    }

字段：
  type        mouse | delay | findImage | input | key
  mouse       action: click | move | down | up；button: left | right | middle；
              x/y 为 FLUTTERVIEW 客户区相对逻辑 px（96 dpi）
  delay       ms：等待毫秒数
  findImage   image 图片路径、threshold 匹配阈值、timeout_ms 超时；暂不实现
  input       text：要发送的文本，逐字符以 WM_CHAR 后台投递（多行的 \n 发成 \r）
  key         key：按键表达式，直接写组合键，如 Ctrl+Shift+A / Alt+F4 / Enter /
              F1 / 单个字符 A；修饰键支持 Ctrl / Alt / Shift / Win；
              action：press(默认) | down | up
  loop        编辑器试跑时整份脚本重复的轮数（DLL 不解析它）
"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import threading
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import filedialog, messagebox, simpledialog, ttk

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# --- 常量 -----------------------------------------------------------------
WM_MOUSEMOVE = 0x0200
WM_CHAR = 0x0102
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_QUIT = 0x0012
SMTO_BLOCK = 0x0001
SMTO_ABORTIFHUNG = 0x0002
KEY_DOWN_BIT = 0x80
WH_MOUSE_LL = 14
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010

MAIN_CLASS = "FLUTTER_RUNNER_WIN32_WINDOW"
VIEW_CLASS = "FLUTTERVIEW"

TYPE_MOUSE = "mouse"
TYPE_DELAY = "delay"
TYPE_FIND = "findImage"
TYPE_INPUT = "input"
TYPE_KEY = "key"
TYPE_LABELS = {TYPE_MOUSE: "鼠标事件", TYPE_DELAY: "延迟事件", TYPE_FIND: "找图事件",
               TYPE_INPUT: "输入事件", TYPE_KEY: "按键事件"}
LABEL_TYPES = {v: k for k, v in TYPE_LABELS.items()}

MOUSE_ACTIONS = {"click": "单击", "move": "移动", "down": "按下", "up": "抬起"}
LABEL_ACTIONS = {v: k for k, v in MOUSE_ACTIONS.items()}
MOUSE_BUTTONS = {"left": "左键", "right": "右键", "middle": "中键"}
LABEL_BUTTONS = {v: k for k, v in MOUSE_BUTTONS.items()}

KEY_ACTIONS = {"press": "单击", "down": "按下", "up": "抬起"}
LABEL_KEY_ACTIONS = {v: k for k, v in KEY_ACTIONS.items()}
MOD_ORDER = ("ctrl", "alt", "shift", "win")
MOD_LABELS = {"ctrl": "Ctrl", "alt": "Alt", "shift": "Shift", "win": "Win"}
MOD_VK = {"ctrl": 0x11, "alt": 0x12, "shift": 0x10, "win": 0x5B}

# 键名 -> 虚拟键码（小写匹配；单字符另外用 VkKeyScanW 兜底）
KEY_VK = {
    "backspace": 0x08, "tab": 0x09, "enter": 0x0D, "return": 0x0D,
    "pause": 0x13, "capslock": 0x14, "esc": 0x1B, "escape": 0x1B,
    "space": 0x20, "pageup": 0x21, "pagedown": 0x22, "end": 0x23, "home": 0x24,
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
    "printscreen": 0x2C, "insert": 0x2D, "delete": 0x2E, "del": 0x2E,
    "numlock": 0x90, "scrolllock": 0x91,
}
for _i in range(1, 25):
    KEY_VK["f%d" % _i] = 0x6F + _i          # F1=0x70 ... F24=0x87
for _c in "abcdefghijklmnopqrstuvwxyz":
    KEY_VK[_c] = ord(_c.upper())
for _d in "0123456789":
    KEY_VK[_d] = ord(_d)

# 需要带「扩展键」标志的虚拟键（方向键、编辑键区等）
EXTENDED_VKS = frozenset({0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                          0x2C, 0x2D, 0x2E, 0x5B, 0x5C, 0x90})

KEY_CHOICES = (["Enter", "Esc", "Tab", "Space", "Backspace", "Delete", "Insert",
                "Home", "End", "PageUp", "PageDown", "Up", "Down", "Left", "Right"]
               + ["F%d" % i for i in range(1, 13)]
               + list("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
               + list("0123456789"))

DELAY_UNITS = {"毫秒": 1, "秒": 1000}   # 单位 -> 换算成毫秒的倍数

ACTION_INDEX = {"move": 0, "down": 1, "up": 2, "click": 3}
BUTTON_INDEX = {"left": 0, "right": 1, "middle": 2}

MAX_EVENTS = 256          # 编辑器上限（DLL 不再限制条数）
MAX_LOOP = 99


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
user32.IsWindow.argtypes = (wintypes.HWND,)
user32.GetClientRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
user32.ClientToScreen.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
user32.ScreenToClient.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
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
user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
user32.PostMessageW.restype = wintypes.BOOL
user32.MapVirtualKeyW.argtypes = (wintypes.UINT, wintypes.UINT)
user32.MapVirtualKeyW.restype = wintypes.UINT
user32.VkKeyScanW.argtypes = (ctypes.c_wchar,)
user32.VkKeyScanW.restype = ctypes.c_short
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.AttachThreadInput.argtypes = (wintypes.DWORD, wintypes.DWORD, wintypes.BOOL)
user32.AttachThreadInput.restype = wintypes.BOOL
user32.GetKeyboardState.argtypes = (ctypes.POINTER(ctypes.c_ubyte),)
user32.GetKeyboardState.restype = wintypes.BOOL
user32.SetKeyboardState.argtypes = (ctypes.POINTER(ctypes.c_ubyte),)
user32.SetKeyboardState.restype = wintypes.BOOL
user32.SendMessageTimeoutW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                       wintypes.LPARAM, wintypes.UINT, wintypes.UINT,
                                       ctypes.POINTER(wintypes.WPARAM))
user32.SendMessageTimeoutW.restype = wintypes.LPARAM

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
    """当前找到的应用窗口（主窗口 + FLUTTERVIEW）。"""

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
            return "未找到应用窗口（先启动应用，再点「刷新」）"
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

    user32.EnumWindows(WNDENUMPROC(on_top), 0)
    if not top:
        return t
    t.main = top[0]

    views = []

    def on_child(hwnd, _lp):
        if get_class(hwnd) == VIEW_CLASS:
            views.append(hwnd)
            return False
        return True

    user32.EnumChildWindows(t.main, WNDENUMPROC(on_child), 0)
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
    """拾取期间盖在目标 view 上的透明遮罩：十字线 + 实时鼠标坐标。"""

    BG = "black"
    IN = "#00E5FF"
    OUT = "#FF8A80"

    def __init__(self, master, view: int, scale: float):
        super().__init__(master)
        self.view = view
        self.scale = scale
        self.overrideredirect(True)
        self.attributes("-topmost", True)
        self.attributes("-transparentcolor", self.BG)
        self.attributes("-alpha", 0.92)
        self.canvas = tk.Canvas(self, background=self.BG, highlightthickness=0, borderwidth=0)
        self.canvas.pack(fill="both", expand=True)
        self._border = self.canvas.create_rectangle(0, 0, 0, 0, outline=self.IN, width=2)
        self._vline = self.canvas.create_line(0, 0, 0, 0, fill=self.IN)
        self._hline = self.canvas.create_line(0, 0, 0, 0, fill=self.IN)
        self._ring = self.canvas.create_rectangle(0, 0, 0, 0, outline=self.OUT, width=2)
        self._tag_bg = self.canvas.create_rectangle(0, 0, 0, 0, fill="#101820", outline=self.IN)
        self._tag = self.canvas.create_text(0, 0, anchor="nw", fill="#FFFFFF",
                                            font=("Consolas", 11, "bold"), text="")
        self._tip = self.canvas.create_text(0, 0, anchor="n", fill="#FFD54F",
                                            font=("Microsoft YaHei UI", 10, "bold"),
                                            text="左键拾取　右键取消")
        self._place()
        self._click_through()
        self.refresh()

    def _place(self) -> None:
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
        self.update()
        hwnd = self.winfo_id()
        if not hwnd:
            return
        style = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                                 style | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)
        user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)

    def refresh(self) -> None:
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


# --- 文本输入（编辑器直接投 WM_CHAR，DLL 不管键盘）-------------------------
def text_preview(text: str, limit: int = 24) -> str:
    """把多行文本压成一行摘要，给表格/日志用。"""
    flat = " ".join((text or "").split())
    if not flat:
        return "(空)"
    return flat if len(flat) <= limit else flat[:limit] + "…"


def post_text(hwnd: int, text: str, stop: threading.Event | None = None) -> int:
    """逐字符以 WM_CHAR 后台投递到窗口（不置前、不动键盘状态）。

    返回成功投递的 UTF-16 码元数；stop 置位时提前中断。
    """
    if not hwnd or not user32.IsWindow(hwnd):
        return 0                     # 0 是 HWND_BROADCAST，不能拿来广播按键
    sent = 0
    for ch in text or "":
        if stop is not None and stop.is_set():
            break
        if ch == "\r":
            continue
        if ch == "\n":
            ch = "\r"          # 多行文本：换行按回车发
        raw = ch.encode("utf-16-le")
        for i in range(0, len(raw), 2):
            code = int.from_bytes(raw[i:i + 2], "little")
            if user32.PostMessageW(hwnd, WM_CHAR, code, 1):
                sent += 1
    return sent


# --- 键盘按键（同样走 PostMessage，不占真实键盘）---------------------------
def resolve_vk(name: str) -> int | None:
    """键名 -> 虚拟键码：先查表，再按单字符用 VkKeyScanW 兜底。"""
    key = (name or "").strip()
    if not key:
        return None
    low = key.lower()
    if low in KEY_VK:
        return KEY_VK[low]
    if len(key) == 1:
        code = user32.VkKeyScanW(key)
        if code != -1:
            return code & 0xFF
        if key.isascii():
            return ord(key.upper())
    return None


def key_text(expr: str, action: str = "press") -> str:
    """给人看的按键文本，如 Ctrl+Shift+A（按下）。"""
    text = str(expr or "").strip()
    act = KEY_ACTIONS.get(action, "单击")
    return text if act == "单击" else "%s（%s）" % (text, act)


def combo_of(ev: dict) -> str:
    """读事件里的按键表达式：新格式 key 就是完整表达式，旧格式把 mods 拼进去。"""
    key = str(ev.get("key", "") or "").strip()
    mods = [m for m in (ev.get("mods") or []) if m in MOD_VK] if "mods" in ev else []
    raw = "+".join([MOD_LABELS[m] for m in MOD_ORDER if m in mods] + [key]).strip("+")
    return normalize_combo(raw) or "Enter"


def _post_key_msg(hwnd: int, vk: int, up: bool, sys: bool) -> bool:
    lparam = 1 | ((user32.MapVirtualKeyW(vk, 0) & 0xFF) << 16)
    if vk in EXTENDED_VKS:
        lparam |= 0x01000000
    if up:
        lparam |= 0xC0000000                 # 位30 前状态 + 位31 转换状态
    if sys:
        msg = WM_SYSKEYUP if up else WM_SYSKEYDOWN
    else:
        msg = WM_KEYUP if up else WM_KEYDOWN
    return bool(user32.PostMessageW(hwnd, msg, vk, lparam))


def _send_key_msg_sync(hwnd: int, vk: int, up: bool, sys: bool) -> bool:
    """同步投递（SendMessageTimeout）：目标处理完消息才返回，保证注入的修饰键
    状态在对方查询 GetKeyState 时仍然有效。"""
    lparam = 1 | ((user32.MapVirtualKeyW(vk, 0) & 0xFF) << 16)
    if vk in EXTENDED_VKS:
        lparam |= 0x01000000
    if up:
        lparam |= 0xC0000000
    if sys:
        msg = WM_SYSKEYUP if up else WM_SYSKEYDOWN
    else:
        msg = WM_KEYUP if up else WM_KEYDOWN
    result = wintypes.WPARAM()
    return bool(user32.SendMessageTimeoutW(hwnd, msg, vk, lparam,
                                           SMTO_BLOCK | SMTO_ABORTIFHUNG, 1000,
                                           ctypes.byref(result)))


def _read_keyboard_state() -> ctypes.Array | None:
    buf = (ctypes.c_ubyte * 256)()
    return buf if user32.GetKeyboardState(buf) else None


def _apply_modifier_state(saved, mods_vk, down: bool) -> bool:
    """把修饰键写进当前线程的键盘状态表（attach 后即目标线程的表）。"""
    if saved is None:
        return False
    buf = (ctypes.c_ubyte * 256)()
    buf[:] = saved[:]
    for vk in mods_vk:
        buf[vk] = KEY_DOWN_BIT if down else 0
    return bool(user32.SetKeyboardState(buf))


def split_combo(expr: str) -> tuple[list[str], str]:
    """把 'Ctrl+Shift+A' 拆成 (['ctrl','shift'], 'A')；有看不懂的修饰段则视为无效。"""
    parts = [p.strip() for p in str(expr or "").split("+")]
    parts = [p for p in parts if p]
    if not parts:
        return [], ""
    mods = set()
    for part in parts[:-1]:
        low = part.lower()
        if low in ("ctrl", "control"):
            mods.add("ctrl")
        elif low in ("alt", "menu"):
            mods.add("alt")
        elif low == "shift":
            mods.add("shift")
        elif low in ("win", "meta", "cmd", "super"):
            mods.add("win")
        else:
            return [], ""                # 未知修饰段：别静默丢掉
    return [m for m in MOD_ORDER if m in mods], parts[-1]


def normalize_combo(expr: str) -> str:
    """统一写法：ctrl+shift+a -> Ctrl+Shift+A；认不出来就原样返回。"""
    mods, key = split_combo(expr)
    if not key:
        return str(expr or "").strip()
    if len(key) == 1 and key.isascii() and key.isalpha():
        key = key.upper()
    return "+".join([MOD_LABELS[m] for m in mods] + [key])


def post_key(hwnd: int, expr: str, action: str = "press") -> int:
    """后台发送按键/组合键（不抢前台），返回成功的消息条数。

    expr 支持 'Ctrl+Shift+A'、'Enter'、'F1'、单个字符。
    带修饰键时先 AttachThreadInput 到目标线程，把修饰键写进它的键盘状态表，
    再同步投递消息——对方处理消息时 GetKeyState 也能看到修饰键按住，组合键才
    生效，而且窗口不用切到前台。

    返回 -1 键名/窗口无效；-3 拿不到目标线程输入状态，只发了裸消息。
    """
    if not hwnd or not user32.IsWindow(hwnd):
        return -1
    mods, key = split_combo(expr)
    vk = resolve_vk(key)
    if vk is None:
        return -1
    order = [MOD_VK[m] for m in mods]
    sys = "alt" in mods
    pairs: list[tuple[int, bool]] = []
    if action in ("press", "down"):
        pairs.extend((mvk, False) for mvk in order)
        pairs.append((vk, False))
    if action in ("press", "up"):
        pairs.append((vk, True))
        pairs.extend((mvk, True) for mvk in reversed(order))

    if not order:                        # 没修饰键：直接投消息，最省事
        sent = 0
        for code, up in pairs:
            if _post_key_msg(hwnd, code, up, sys):
                sent += 1
        return sent

    tid = user32.GetWindowThreadProcessId(hwnd, None)
    me = kernel32.GetCurrentThreadId()
    attached = False
    if tid == me:                        # 同线程：状态表本来就是自己的
        attached = True
    elif tid:
        attached = bool(user32.AttachThreadInput(me, tid, True))
    if not attached:
        sent = 0
        for code, up in pairs:           # 拿不到对方输入状态，只能裸发消息
            if _post_key_msg(hwnd, code, up, sys):
                sent += 1
        return -3 if sent else 0

    saved = _read_keyboard_state()
    sent = 0
    try:
        _apply_modifier_state(saved, order, True)
        for code, up in pairs:
            if _send_key_msg_sync(hwnd, code, up, sys):
                sent += 1
    finally:
        if saved is not None:
            user32.SetKeyboardState(saved)
        if tid and tid != me:
            user32.AttachThreadInput(me, tid, False)
    return sent


# --- DLL（纯执行库）-------------------------------------------------------
def resolve_dll() -> str:
    env = os.environ.get("PP_SCRIPT_DLL", "").strip()
    if env:
        return env
    appdata = os.environ.get("APPDATA", "")
    candidate = os.path.join(appdata, "TTVoice", "plugins", "script-engine", "script-engine.dll")
    if os.path.isfile(candidate):
        return candidate
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(here, "plugin", "script-engine", "script-engine.dll")


def plugin_enabled(dll_path: str) -> bool:
    """读同目录 plugin.json 的 enabled；没有清单视为启用。"""
    manifest = os.path.join(os.path.dirname(dll_path), "plugin.json")
    try:
        with open(manifest, "r", encoding="utf-8-sig") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return True
    return bool(data.get("enabled", True))


class ScriptDll:
    """ctypes wrapper around script-engine.dll's PP_* exports."""

    def __init__(self):
        self.dll = None
        self.path = ""

    def load(self, path: str) -> bool:
        dll = ctypes.WinDLL(path)
        dll.PP_Mouse.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int)
        dll.PP_Mouse.restype = ctypes.c_int
        dll.PP_Delay.argtypes = (ctypes.c_int,)
        dll.PP_Delay.restype = ctypes.c_int
        dll.PP_FindImage.argtypes = (wintypes.HWND, ctypes.c_char_p,
                                     ctypes.c_double, ctypes.c_int)
        dll.PP_FindImage.restype = ctypes.c_int
        dll.PP_Version.argtypes = ()
        dll.PP_Version.restype = ctypes.c_int
        self.dll = dll
        self.path = path
        return True

    @property
    def ready(self) -> bool:
        return self.dll is not None


# --- 事件编辑对话框 -------------------------------------------------------
class EventDialog(tk.Toplevel):
    def __init__(self, master, event: dict, on_pick, title: str = "事件"):
        super().__init__(master)
        self.title(title)
        self.resizable(False, False)
        self.result = None
        self._on_pick = on_pick

        ev = dict(event or {})
        self.type_var = tk.StringVar(value=TYPE_LABELS.get(ev.get("type", TYPE_MOUSE), "鼠标事件"))
        self.action_var = tk.StringVar(value=MOUSE_ACTIONS.get(ev.get("action", "click"), "单击"))
        self.button_var = tk.StringVar(value=MOUSE_BUTTONS.get(ev.get("button", "left"), "左键"))
        self.x_var = tk.StringVar(value=self._fmt(ev.get("x", 0)))
        self.y_var = tk.StringVar(value=self._fmt(ev.get("y", 0)))
        ms0 = float(ev.get("ms", 500) or 0)
        unit0 = "秒" if ms0 >= 1000 and ms0 % 1000 == 0 else "毫秒"
        self._last_unit = unit0
        self.ms_var = tk.StringVar(
            value=self._num_text(ms0 / DELAY_UNITS[unit0]))
        self.unit_var = tk.StringVar(value=unit0)
        self.image_var = tk.StringVar(value=str(ev.get("image", "")))
        self.threshold_var = tk.StringVar(value=str(ev.get("threshold", 0.9)))
        self.timeout_var = tk.StringVar(value=str(ev.get("timeout_ms", 3000)))
        self.text_value = str(ev.get("text", "") or "")
        self.key_var = tk.StringVar(value=combo_of(ev))
        self.key_action_var = tk.StringVar(
            value=KEY_ACTIONS.get(ev.get("action", "press"), "单击"))
        self.note_var = tk.StringVar(value=str(ev.get("remark", "") or ""))

        body = ttk.Frame(self, padding=(12, 10))
        body.grid(row=0, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)

        head = ttk.Frame(body)
        head.grid(row=0, column=0, sticky="ew")
        ttk.Label(head, text="类型", width=9, anchor="w").pack(side="left")
        type_box = ttk.Combobox(head, textvariable=self.type_var, width=12, state="readonly",
                                values=list(TYPE_LABELS.values()))
        type_box.pack(side="left")
        type_box.bind("<<ComboboxSelected>>", lambda _e: self._on_type_change())

        # 鼠标
        self.mouse_frame = ttk.LabelFrame(body, text="鼠标", padding=(10, 4))
        self.mouse_frame.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        self.mouse_frame.columnconfigure(0, weight=1)
        self._row(self.mouse_frame, 0, "动作", combo=self.action_var,
                  values=list(MOUSE_ACTIONS.values()))
        self._row(self.mouse_frame, 1, "按键", combo=self.button_var,
                  values=list(MOUSE_BUTTONS.values()))
        pos = ttk.Frame(self.mouse_frame)
        pos.grid(row=2, column=0, sticky="ew", pady=3)
        ttk.Label(pos, text="X / Y", width=9, anchor="w").pack(side="left")
        ttk.Entry(pos, textvariable=self.x_var, width=8).pack(side="left")
        ttk.Entry(pos, textvariable=self.y_var, width=8).pack(side="left", padx=(4, 0))
        ttk.Button(pos, text="拾取坐标", width=10, command=self._pick_xy).pack(
            side="left", padx=(8, 0))

        # 延迟
        self.delay_frame = ttk.LabelFrame(body, text="延迟", padding=(10, 4))
        self.delay_frame.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        self.delay_frame.columnconfigure(0, weight=1)
        dline = ttk.Frame(self.delay_frame)
        dline.grid(row=0, column=0, sticky="ew", pady=3)
        ttk.Label(dline, text="时长", width=9, anchor="w").pack(side="left")
        ttk.Entry(dline, textvariable=self.ms_var, width=10).pack(side="left")
        self.unit_box = ttk.Combobox(dline, textvariable=self.unit_var, width=6,
                                     state="readonly", values=list(DELAY_UNITS))
        self.unit_box.pack(side="left", padx=(6, 0))
        self.unit_box.bind("<<ComboboxSelected>>", lambda _e: self._on_unit_change())
        self.delay_hint = ttk.Label(self.delay_frame, text="", foreground="#777")
        self.delay_hint.grid(row=1, column=0, sticky="w", padx=(9, 0))
        self.ms_var.trace_add("write", lambda *_: self._update_delay_hint())
        self._update_delay_hint()

        # 找图
        self.find_frame = ttk.LabelFrame(body, text="找图（暂未实现）", padding=(10, 4))
        self.find_frame.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        self.find_frame.columnconfigure(0, weight=1)
        img = ttk.Frame(self.find_frame)
        img.grid(row=0, column=0, sticky="ew", pady=3)
        ttk.Label(img, text="图片", width=9, anchor="w").pack(side="left")
        ttk.Entry(img, textvariable=self.image_var).pack(side="left", fill="x", expand=True)
        ttk.Button(img, text="浏览", width=6, command=self._browse_image).pack(
            side="left", padx=(6, 0))
        self._row(self.find_frame, 1, "阈值", entry=self.threshold_var)
        self._row(self.find_frame, 2, "超时", entry=self.timeout_var, suffix="ms")

        # 输入
        self.input_frame = ttk.LabelFrame(body, text="输入文本", padding=(10, 4))
        self.input_frame.grid(row=4, column=0, sticky="ew", pady=(8, 0))
        self.input_frame.columnconfigure(0, weight=1)
        self.text_box = tk.Text(self.input_frame, height=4, width=44, wrap="word",
                                font=("Microsoft YaHei UI", 9), relief="solid", borderwidth=1)
        self.text_box.grid(row=0, column=0, sticky="ew", pady=3)
        self.text_box.insert("1.0", self.text_value)
        ttk.Label(self.input_frame, foreground="#777",
                  text="逐字符以 WM_CHAR 投递到目标窗口；多行按回车发送，中英文均可").grid(
            row=1, column=0, sticky="w")

        # 按键 / 组合键
        self.key_frame = ttk.LabelFrame(body, text="键盘按键 / 组合键", padding=(10, 4))
        self.key_frame.grid(row=5, column=0, sticky="ew", pady=(8, 0))
        self.key_frame.columnconfigure(0, weight=1)
        key_line = ttk.Frame(self.key_frame)
        key_line.grid(row=0, column=0, sticky="ew", pady=3)
        ttk.Label(key_line, text="按键", width=9, anchor="w").pack(side="left")
        key_box = ttk.Combobox(key_line, textvariable=self.key_var, width=18,
                               values=KEY_CHOICES)
        key_box.pack(side="left")
        key_box.bind("<<ComboboxSelected>>", lambda _e: self._update_key_hint())
        ttk.Label(key_line, foreground="#777",
                  text="可直接写组合键：Ctrl+Shift+A / Alt+F4 / F1 / 单个字符").pack(
            side="left", padx=(8, 0))
        act_line = ttk.Frame(self.key_frame)
        act_line.grid(row=1, column=0, sticky="ew", pady=3)
        ttk.Label(act_line, text="动作", width=9, anchor="w").pack(side="left")
        act_box = ttk.Combobox(act_line, textvariable=self.key_action_var, width=10,
                               state="readonly", values=list(KEY_ACTIONS.values()))
        act_box.pack(side="left")
        act_box.bind("<<ComboboxSelected>>", lambda _e: self._update_key_hint())
        self.key_hint = ttk.Label(self.key_frame, text="", foreground="#777")
        self.key_hint.grid(row=2, column=0, sticky="w", padx=(9, 0))
        self.key_var.trace_add("write", lambda *_: self._update_key_hint())
        self._update_key_hint()

        note = ttk.LabelFrame(body, text="备注", padding=(10, 4))
        note.grid(row=6, column=0, sticky="ew", pady=(8, 0))
        note.columnconfigure(0, weight=1)
        ttk.Entry(note, textvariable=self.note_var).grid(row=0, column=0, sticky="ew", pady=3)

        foot = ttk.Frame(body)
        foot.grid(row=7, column=0, sticky="ew", pady=(12, 0))
        foot.columnconfigure(0, weight=1)
        ttk.Label(foot, text="Enter 确定　Esc 取消", foreground="#888").grid(row=0, column=0, sticky="w")
        ttk.Button(foot, text="确定", width=10, command=self._ok).grid(row=0, column=1, padx=(0, 6))
        ttk.Button(foot, text="取消", width=10, command=self.destroy).grid(row=0, column=2)

        self.transient(master)
        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.bind("<Return>", lambda _e: self._ok())
        self.bind("<Escape>", lambda _e: self.destroy())
        self._on_type_change()
        self._center_on(master)

    @staticmethod
    def _fmt(v) -> str:
        v = round(float(v), 1)
        return str(int(v)) if v.is_integer() else str(v)

    @staticmethod
    def _num_text(v: float) -> str:
        """数字转文本：去掉多余的 0（0.500 -> 0.5，2.000 -> 2）。"""
        text = ("%.3f" % float(v)).rstrip("0").rstrip(".")
        return text or "0"

    def _delay_ms(self) -> float | None:
        """按当前单位把输入换算成毫秒；输入非法返回 None。"""
        try:
            return float(self.ms_var.get() or 0) * DELAY_UNITS.get(self.unit_var.get(), 1)
        except ValueError:
            return None

    def _on_unit_change(self) -> None:
        """切换单位时按同一时长换算数值（500 毫秒 <-> 0.5 秒）。"""
        cur = self.unit_var.get()
        if cur == self._last_unit:
            return
        ms = self._delay_ms()
        self._last_unit = cur
        if ms is None:
            self._update_delay_hint()
            return
        self.ms_var.set(self._num_text(ms / DELAY_UNITS.get(cur, 1)))
        self._update_delay_hint()

    @staticmethod
    def _hms_text(ms: float) -> str:
        """毫秒 -> 时分秒，如 3725000 -> 1时2分5秒。"""
        total = int(round(max(0.0, float(ms))))
        hours, rest = divmod(total, 3600000)
        minutes, sec_ms = divmod(rest, 60000)
        return "%d时%d分%s秒" % (hours, minutes, EventDialog._num_text(sec_ms / 1000.0))

    def _update_delay_hint(self) -> None:
        ms = self._delay_ms()
        if ms is None:
            text = "实际时间：请输入数字"
        else:
            text = "实际时间：%s" % self._hms_text(ms)
        try:
            self.delay_hint.configure(text=text)
        except tk.TclError:
            pass

    def _row(self, parent, row, label, combo=None, entry=None, suffix="", values=None):
        line = ttk.Frame(parent)
        line.grid(row=row, column=0, sticky="ew", pady=3)
        ttk.Label(line, text=label, width=9, anchor="w").pack(side="left")
        if combo is not None:
            ttk.Combobox(line, textvariable=combo, width=10, state="readonly",
                         values=values or []).pack(side="left")
        elif entry is not None:
            ttk.Entry(line, textvariable=entry, width=12).pack(side="left")
        if suffix:
            ttk.Label(line, text=suffix, foreground="#777").pack(side="left", padx=(8, 0))

    def _on_type_change(self) -> None:
        t = LABEL_TYPES.get(self.type_var.get(), TYPE_MOUSE)
        self._set_frame_enabled(self.mouse_frame, t == TYPE_MOUSE)
        self._set_frame_enabled(self.delay_frame, t == TYPE_DELAY)
        self._set_frame_enabled(self.find_frame, t == TYPE_FIND)
        self._set_frame_enabled(self.input_frame, t == TYPE_INPUT)
        self._set_frame_enabled(self.key_frame, t == TYPE_KEY)

    def _update_key_hint(self) -> None:
        expr = self.key_var.get().strip()
        action = LABEL_KEY_ACTIONS.get(self.key_action_var.get(), "press")
        mods, key = split_combo(expr)
        if not expr:
            text = "按键不能为空"
        elif not key or resolve_vk(key) is None:
            text = "无法识别的按键：%s" % expr
        else:
            text = "将发送：%s（后台投递，会临时注入修饰键状态）" % key_text(
                normalize_combo(expr), action)
        try:
            self.key_hint.configure(text=text)
        except tk.TclError:
            pass

    @staticmethod
    def _set_frame_enabled(widget, enabled: bool) -> None:
        """ttk.LabelFrame 本身没有 state，只对内部控件递归设置。

        Combobox / Text 等的原状态会被记住：下拉框恢复 readonly，而不是被
        改成可编辑（否则用户能输入任意文本，确定时又静默回落默认值）。
        """
        if isinstance(widget, ttk.Combobox):
            if enabled:
                state = getattr(widget, "_pp_state", None)   # 没被禁用过则保持原样
            else:
                cur = str(widget.cget("state"))
                if cur not in ("", "disabled"):
                    widget._pp_state = cur
                state = "disabled"
        else:
            state = "normal" if enabled else "disabled"
        if state is not None:
            try:
                widget.configure(state=state)
            except tk.TclError:
                pass
        for child in widget.winfo_children():
            EventDialog._set_frame_enabled(child, enabled)

    def _pick_xy(self):
        def fill(lx, ly):
            if not self.winfo_exists():
                return
            self.x_var.set(self._fmt(lx))
            self.y_var.set(self._fmt(ly))
        self._on_pick(fill)

    def _browse_image(self) -> None:
        path = filedialog.askopenfilename(parent=self, title="选择找图模板",
                                          filetypes=[("图片", "*.png;*.bmp;*.jpg;*.jpeg"),
                                                     ("所有文件", "*.*")])
        if path:
            self.image_var.set(path)

    def _center_on(self, master) -> None:
        self.update_idletasks()
        mw = master.winfo_width() or master.winfo_screenwidth()
        mh = master.winfo_height() or master.winfo_screenheight()
        mx = master.winfo_rootx()
        my = master.winfo_rooty()
        self.geometry("+%d+%d" % (mx + (mw - self.winfo_width()) // 2,
                                  my + (mh - self.winfo_height()) // 2))

    def _ok(self) -> None:
        t = LABEL_TYPES.get(self.type_var.get(), TYPE_MOUSE)
        try:
            if t == TYPE_MOUSE:
                ev = {
                    "type": TYPE_MOUSE,
                    "action": LABEL_ACTIONS.get(self.action_var.get(), "click"),
                    "button": LABEL_BUTTONS.get(self.button_var.get(), "left"),
                    "x": self._num(self.x_var.get()),
                    "y": self._num(self.y_var.get()),
                }
            elif t == TYPE_DELAY:
                ms = self._delay_ms()
                if ms is None:
                    raise ValueError("ms")
                ev = {"type": TYPE_DELAY, "ms": int(round(ms))}
            elif t == TYPE_INPUT:
                text = self.text_box.get("1.0", "end-1c").rstrip("\n")
                if not text.strip():
                    messagebox.showwarning("内容为空", "请输入要发送的文本。", parent=self)
                    return
                ev = {"type": TYPE_INPUT, "text": text}
            elif t == TYPE_KEY:
                expr = self.key_var.get().strip()
                if not expr:
                    messagebox.showwarning("按键为空",
                                           "请输入按键，例如 Enter / F1 / Ctrl+A。", parent=self)
                    return
                _mods, key = split_combo(expr)
                if not key or resolve_vk(key) is None:
                    messagebox.showwarning(
                        "按键无法识别",
                        "看不懂：%s\n示例：Enter / F1 / A / Ctrl+Shift+A（修饰键只支持 "
                        "Ctrl/Alt/Shift/Win）" % expr, parent=self)
                    return
                ev = {"type": TYPE_KEY, "key": normalize_combo(expr),
                      "action": LABEL_KEY_ACTIONS.get(self.key_action_var.get(), "press")}
            else:
                ev = {
                    "type": TYPE_FIND,
                    "image": self.image_var.get().strip(),
                    "threshold": float(self.threshold_var.get() or 0.9),
                    "timeout_ms": int(float(self.timeout_var.get() or 0)),
                    "x": self._num(self.x_var.get()),
                    "y": self._num(self.y_var.get()),
                }
        except ValueError:
            messagebox.showerror("格式错误", "数值字段格式不正确", parent=self)
            return
        ev["remark"] = self.note_var.get().strip()
        self.result = ev
        self.destroy()

    @staticmethod
    def _num(text: str) -> int | float:
        v = float(text or 0)
        return int(v) if v.is_integer() else v


# --- 主窗口 ---------------------------------------------------------------
class App:
    COLUMNS = (
        ("idx", "#", 44, "center"),
        ("type", "类型", 90, "center"),
        ("detail", "参数", 480, "w"),
        ("note", "备注", 240, "w"),
    )

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("pplugin 脚本事件编辑器")
        self.scale = self._ui_scale()
        self.root.geometry("%dx%d" % (int(960 * self.scale), int(720 * self.scale)))
        self.root.minsize(int(880 * self.scale), int(600 * self.scale))

        self.scripts_dir = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "scripts")
        os.makedirs(self.scripts_dir, exist_ok=True)

        self.target = Target()
        self.events: list[dict] = []
        self.current_name = ""
        self.dirty = False

        self.pick_queue: queue.Queue | None = None
        self.picker: ClickPicker | None = None
        self.pick_cb = None
        self.overlay: PickOverlay | None = None
        self._cursor_job = None

        self.msg_queue: queue.Queue = queue.Queue()
        self.stop_evt = threading.Event()
        self.running = False

        self.script_dll = ScriptDll()
        self.name_var = tk.StringVar(value="活动")
        self.loop_var = tk.StringVar(value="1")
        self.status_var = tk.StringVar(value="")

        self._build()
        self.refresh_target(quiet=True)
        self._status(self.target.describe())
        self.refresh_script_list(load_first=True)
        self.render_table()
        self._log("dll: %s%s" % (resolve_dll(),
                                 "" if self.script_dll.ready else "（尚未加载）"))
        self.root.after(100, self._poll_messages)

    def _ui_scale(self) -> float:
        try:
            return max(1.0, float(self.root.winfo_fpixels("1i")) / 96.0)
        except tk.TclError:
            return 1.0

    # -- UI ----------------------------------------------------------------
    def _init_style(self) -> None:
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

        top = ttk.Frame(outer)
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
        ttk.Button(files, text="改名", width=6, command=self.rename_script).pack(side="left", padx=(4, 0))
        ttk.Button(files, text="删除", width=6, command=self.delete_script).pack(side="left", padx=(4, 0))

        bottom = ttk.Frame(outer, padding=(0, 6))
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

        paned = ttk.Panedwindow(outer, orient="vertical")
        paned.pack(fill="both", expand=True)
        self.paned = paned

        table_box = ttk.LabelFrame(paned, text="事件表", padding=(6, 4))
        paned.add(table_box, weight=4)
        tools = ttk.Frame(table_box)
        tools.pack(fill="x", pady=(0, 4))
        ttk.Button(tools, text="添加鼠标", command=lambda: self.add_event(TYPE_MOUSE)).pack(side="left")
        ttk.Button(tools, text="添加延迟", command=lambda: self.add_event(TYPE_DELAY)).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="添加找图", command=lambda: self.add_event(TYPE_FIND)).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="添加输入", command=lambda: self.add_event(TYPE_INPUT)).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="添加按键", command=lambda: self.add_event(TYPE_KEY)).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="修改", width=6, command=self.edit_selected).pack(side="left", padx=(8, 0))
        ttk.Button(tools, text="删除行", width=7, command=self.delete_selected).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="上移", width=6, command=lambda: self.move_row(-1)).pack(
            side="left", padx=(4, 0))
        ttk.Button(tools, text="下移", width=6, command=lambda: self.move_row(1)).pack(
            side="left", padx=(4, 0))
        self.count_var = tk.StringVar(value="")
        ttk.Label(tools, textvariable=self.count_var, foreground="#555").pack(side="right")

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

        logf = ttk.LabelFrame(paned, text="脚本日志", padding=(6, 4))
        paned.add(logf, weight=1)
        log_wrap = ttk.Frame(logf)
        log_wrap.pack(fill="both", expand=True)
        self.log_box = tk.Text(log_wrap, height=6, wrap="none", state="disabled",
                               font=("Consolas", 9), relief="flat",
                               background="#141414", foreground="#B3A981")
        log_scroll = ttk.Scrollbar(log_wrap, orient="vertical", command=self.log_box.yview)
        log_xscroll = ttk.Scrollbar(log_wrap, orient="horizontal", command=self.log_box.xview)
        self.log_box.configure(yscrollcommand=log_scroll.set, xscrollcommand=log_xscroll.set)
        self.log_box.grid(row=0, column=0, sticky="nsew")
        log_scroll.grid(row=0, column=1, sticky="ns")
        log_xscroll.grid(row=1, column=0, sticky="ew")
        log_wrap.rowconfigure(0, weight=1)
        log_wrap.columnconfigure(0, weight=1)
        self.log_box.tag_config("warn", foreground="#FFB454")

        self.root.bind("<Control-s>", lambda _e: self.save_script())
        self.root.bind("<Control-n>", lambda _e: self.new_script())
        self.root.bind("<F2>", lambda _e: self.rename_script())
        self.root.bind("<F5>", lambda _e: self.do_run())
        self.name_var.trace_add("write", lambda *_: self._mark_dirty())
        self.loop_var.trace_add("write", lambda *_: self._mark_dirty())
        self._update_title()
        self.root.after_idle(self._place_sash)

    def _mark_dirty(self) -> None:
        self.dirty = True
        self._update_title()

    def _update_title(self) -> None:
        name = self.current_name or "未保存"
        star = " *" if self.dirty else ""
        self.root.title("pplugin 脚本事件编辑器 - %s%s" % (name, star))

    def _place_sash(self, tries: int = 0) -> None:
        h = self.paned.winfo_height()
        if h < 240 and tries < 20:
            self.root.after(60, lambda: self._place_sash(tries + 1))
            return
        try:
            self.paned.sashpos(0, max(120, int(h * 0.64)))
        except tk.TclError:
            pass

    # -- 目标窗口 ----------------------------------------------------------
    def refresh_target(self, quiet: bool = False) -> None:
        self.target = find_target()
        if not quiet:
            self._status(self.target.describe())
        elif not self.target.ok:
            self._status(self.target.describe())

    def _ensure_target(self) -> bool:
        if self.target.ok:
            return True
        self.refresh_target(quiet=True)
        if not self.target.ok:
            messagebox.showwarning("找不到应用窗口",
                                   "没找到 FLUTTERVIEW 子窗口。先启动应用，再点「刷新应用」。")
            return False
        return True

    # -- 表格 --------------------------------------------------------------
    @staticmethod
    def _event_detail(ev: dict) -> str:
        t = ev.get("type", TYPE_MOUSE)
        if t == TYPE_DELAY:
            return "%d ms" % int(ev.get("ms", 0))
        if t == TYPE_FIND:
            return "%s  ≥%.2f  ≤%dms" % (ev.get("image", "") or "(未选图)",
                                          float(ev.get("threshold", 0.9)),
                                          int(ev.get("timeout_ms", 0)))
        if t == TYPE_INPUT:
            return "输入「%s」" % text_preview(ev.get("text", ""))
        if t == TYPE_KEY:
            return key_text(combo_of(ev), ev.get("action", "press"))
        return "%s %s (%s, %s)" % (
            MOUSE_BUTTONS.get(ev.get("button", "left"), "左键"),
            MOUSE_ACTIONS.get(ev.get("action", "click"), "单击"),
            fmt_coord(ev.get("x", 0)), fmt_coord(ev.get("y", 0)))

    def render_table(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, ev in enumerate(self.events, 1):
            self.tree.insert("", "end", iid=str(i - 1),
                             tags=(self.tree_tag_odd if i % 2 else self.tree_tag_even,),
                             values=(i, TYPE_LABELS.get(ev.get("type", TYPE_MOUSE), "鼠标事件"),
                                     self._event_detail(ev), ev.get("remark", "") or ""))
        over = len(self.events) > MAX_EVENTS
        self.count_var.set("共 %d 行%s" % (len(self.events),
                                          "（超过编辑器上限 %d）" % MAX_EVENTS if over else ""))
        self._update_title()

    def _selected_index(self) -> int | None:
        sel = self.tree.selection()
        if not sel:
            return None
        try:
            return int(sel[0])
        except (TypeError, ValueError):
            return None

    def add_event(self, type_key: str) -> None:
        default = {"type": TYPE_MOUSE, "action": "click", "button": "left", "x": 0, "y": 0,
                   "remark": ""}
        if type_key == TYPE_DELAY:
            default = {"type": TYPE_DELAY, "ms": 500, "remark": ""}
        elif type_key == TYPE_FIND:
            default = {"type": TYPE_FIND, "image": "", "threshold": 0.9,
                       "timeout_ms": 3000, "x": 0, "y": 0, "remark": ""}
        elif type_key == TYPE_INPUT:
            default = {"type": TYPE_INPUT, "text": "", "remark": ""}
        elif type_key == TYPE_KEY:
            default = {"type": TYPE_KEY, "key": "Enter", "action": "press", "remark": ""}
        dialog = EventDialog(self.root, default, self._begin_pick, title="添加事件")
        self.root.wait_window(dialog)
        if dialog.result is not None:
            self.events.append(dialog.result)
            self.dirty = True
            self.render_table()

    def edit_selected(self) -> None:
        idx = self._selected_index()
        if idx is None or idx >= len(self.events):
            return
        dialog = EventDialog(self.root, self.events[idx], self._begin_pick, title="修改事件")
        self.root.wait_window(dialog)
        if dialog.result is not None:
            self.events[idx] = dialog.result
            self.dirty = True
            self.render_table()

    def delete_selected(self) -> None:
        idx = self._selected_index()
        if idx is None or idx >= len(self.events):
            return
        del self.events[idx]
        self.dirty = True
        self.render_table()

    def move_row(self, delta: int) -> None:
        idx = self._selected_index()
        if idx is None:
            return
        j = idx + delta
        if not (0 <= j < len(self.events)):
            return
        self.events[idx], self.events[j] = self.events[j], self.events[idx]
        self.dirty = True
        self.render_table()
        self.tree.selection_set(str(j))

    # -- 拾取 --------------------------------------------------------------
    def _begin_pick(self, callback) -> None:
        if self.picker is not None:
            return
        if not self._ensure_target():
            return
        self.pick_queue = queue.Queue()
        self.pick_cb = callback
        self.picker = ClickPicker(self.target.view, self.target.scale, self.pick_queue)
        self._show_overlay()
        self.picker.start()
        self._poll_pick()

    def _poll_pick(self) -> None:
        if self.pick_queue is None:
            return
        try:
            item = self.pick_queue.get_nowait()
        except queue.Empty:
            self.root.after(30, self._poll_pick)
            return
        kind = item[0]
        if kind == "pick":
            _px, _py, lx, ly = item[1], item[2], item[3], item[4]
            cb = self.pick_cb
            self._end_pick()
            if cb is not None:
                cb(lx, ly)
        elif kind == "cancel":
            self._end_pick()
        else:
            self._end_pick()
            self._log("pick: 钩子安装失败 err=%s" % (item[1] if len(item) > 1 else "?"),
                      tag="warn")

    def _end_pick(self) -> None:
        self._hide_overlay()
        if self.picker is not None:
            self.picker.stop()
        self.picker = None
        self.pick_queue = None
        self.pick_cb = None

    def _show_overlay(self) -> None:
        self._hide_overlay()
        try:
            self.overlay = PickOverlay(self.root, self.target.view, self.target.scale)
        except tk.TclError:
            self.overlay = None
        self._cursor_tick()

    def _cursor_tick(self) -> None:
        self._cursor_job = None
        if self.picker is None:
            return
        if self.overlay is not None:
            try:
                self.overlay.refresh()
            except tk.TclError:
                self.overlay = None
        self._cursor_job = self.root.after(30, self._cursor_tick)

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
        self.loop_var.set(str(data.get("loop", 1)))
        self.events = [self._clean_event(e) for e in data.get("events", []) if isinstance(e, dict)]
        self.dirty = False
        self.script_combo.set(name)
        self.render_table()
        self._status("已载入 %s（%d 行）" % (name, len(self.events)))
        self._log("file: %s" % path)

    @staticmethod
    def _clean_event(ev: dict) -> dict:
        t = ev.get("type", TYPE_MOUSE)
        if t == TYPE_DELAY:
            return {"type": TYPE_DELAY, "ms": int(ev.get("ms", 0) or 0),
                    "remark": str(ev.get("remark", "") or "").strip()}
        if t == TYPE_FIND:
            return {"type": TYPE_FIND, "image": str(ev.get("image", "") or ""),
                    "threshold": float(ev.get("threshold", 0.9) or 0.9),
                    "timeout_ms": int(ev.get("timeout_ms", 0) or 0),
                    "x": ev.get("x", 0), "y": ev.get("y", 0),
                    "remark": str(ev.get("remark", "") or "").strip()}
        if t == TYPE_INPUT:
            return {"type": TYPE_INPUT, "text": str(ev.get("text", "") or ""),
                    "remark": str(ev.get("remark", "") or "").strip()}
        if t == TYPE_KEY:
            action = ev.get("action", "press")
            return {"type": TYPE_KEY,
                    "key": combo_of(ev),
                    "action": action if action in KEY_ACTIONS else "press",
                    "remark": str(ev.get("remark", "") or "").strip()}
        return {"type": TYPE_MOUSE, "action": ev.get("action", "click"),
                "button": ev.get("button", "left"), "x": ev.get("x", 0), "y": ev.get("y", 0),
                "remark": str(ev.get("remark", "") or "").strip()}

    def _collect(self) -> dict:
        return {
            "version": 2,
            "name": self.name_var.get().strip() or self.current_name or "script",
            "loop": min(MAX_LOOP, max(1, self._int(self.loop_var.get(), 1))),
            "events": [self._clean_event(e) for e in self.events],
        }

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
        self.loop_var.set("1")
        self.events = []
        self.dirty = True
        self.render_table()
        self._status("新建脚本：加事件后点「保存」")

    def save_script(self) -> None:
        name = self.current_name or self.name_var.get().strip()
        if not name:
            self.save_script_as()
            return
        self._write(name)

    @staticmethod
    def _safe_name(name: str) -> str:
        """去掉 Windows 文件名非法字符，并去掉结尾的点/空格。"""
        name = (name or "").replace("\\", "").replace("/", "")
        for ch in ':*?"<>|':
            name = name.replace(ch, "")
        return name.strip().rstrip(". ")

    def rename_script(self) -> None:
        """改当前脚本的名称（重命名 json 文件，并同步文件内的 name 字段）。"""
        if self.running:
            return
        old = self.current_name
        if not old:
            messagebox.showinfo("修改脚本名称", "先选择或保存一个脚本，再改名。")
            return
        new = simpledialog.askstring("修改脚本名称", "新名称：", initialvalue=old,
                                     parent=self.root)
        if new is None:
            return
        new = self._safe_name(new)
        if not new:
            messagebox.showwarning("名称无效", "名称不能为空，且不能含 \\ / : * ? \" < > | 等字符。")
            return
        if new == old:
            return
        old_path = self._script_path(old)
        new_path = self._script_path(new)
        if os.path.exists(new_path) and not messagebox.askyesno(
                "名称已存在", "「%s」已存在，覆盖它？" % new, parent=self.root):
            return
        was_dirty = self.dirty
        try:
            os.replace(old_path, new_path)
        except OSError as exc:
            messagebox.showerror("改名失败", "%s\n%s" % (old_path, exc))
            return
        self.current_name = new
        self.name_var.set(new)
        self.refresh_script_list()
        self.script_combo.set(new)
        self._log("file: %s -> %s" % (old_path, new_path))
        if was_dirty:
            # 有未保存修改时不回写文件，等用户点「保存」再同步 name 字段
            self.dirty = True
            self._update_title()
            self._status("已改名 %s -> %s（有未保存修改，记得保存）" % (old, new))
            return
        self._write(new)
        self._status("已改名 %s -> %s" % (old, new))

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
        self._update_title()
        self._status("已保存 %s" % name)
        self._log("file: 已保存 %s" % path)

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

    # -- 执行（ctypes 调 script-engine.dll，逐事件）-------------------------------
    def _ensure_dll(self) -> bool:
        if self.script_dll.ready:
            return True
        path = resolve_dll()
        if not os.path.isfile(path):
            messagebox.showerror(
                "找不到 script-engine.dll",
                "先编译执行库：\n  python build_plugin.py script-engine\n或设置环境变量 PP_SCRIPT_DLL 指向 script-engine.dll。\n\n期望路径：\n%s" % path)
            return False
        if not plugin_enabled(path):
            messagebox.showwarning(
                "已被禁用",
                "plugin.json 里 enabled=false，script-engine 不会加载：\n%s"
                % os.path.dirname(path))
            return False
        try:
            self.script_dll.load(path)
        except OSError as exc:
            messagebox.showerror("加载 script-engine.dll 失败", "%s\n%s" % (path, exc))
            return False
        self._log("dll: 已加载 %s (v%d)" % (path, self.script_dll.dll.PP_Version()))
        return True

    def do_run(self) -> None:
        self._start_job(list(self.events), self._int(self.loop_var.get(), 1), 1)

    def do_run_selected(self) -> None:
        idx = self._selected_index()
        if idx is None or idx >= len(self.events):
            self._status("先选中一行")
            return
        self._start_job([dict(self.events[idx])], 1, idx + 1)

    def _start_job(self, events: list[dict], loop: int, first_index: int) -> None:
        if self.running:
            self._log("script: run ignored, previous run still active")
            return
        if not events:
            messagebox.showinfo("提示", "事件表是空的")
            return
        if not self._ensure_target():
            return
        if not self._ensure_dll():
            return
        loop = min(MAX_LOOP, max(1, loop))
        self.stop_evt = threading.Event()
        self.running = True
        self.run_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        threading.Thread(target=self._job_thread,
                         args=(events, loop, first_index, self.target.view, self.target.scale),
                         daemon=True).start()

    def do_stop(self) -> None:
        self.stop_evt.set()
        self._status("正在停止…")

    def _job_thread(self, events, loop, first_index, view, scale) -> None:
        started = time.time()
        try:
            self.msg_queue.put("script: run events=%d loop=%d view %s"
                               % (len(events), loop, view))
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
                    self._run_event(ev, view, scale)
                if aborted:
                    break
            ms = int((time.time() - started) * 1000)
            self.msg_queue.put("script: %s in %dms" % ("stopped" if aborted else "done", ms))
            self.msg_queue.put("__DONE__")
        except Exception as exc:
            self.msg_queue.put("__ERROR__%s" % exc)

    def _run_event(self, ev: dict, view: int, scale: float) -> None:
        dll = self.script_dll.dll
        t = ev.get("type", TYPE_MOUSE)
        if t == TYPE_DELAY:
            ms = int(ev.get("ms", 0) or 0)
            # 分片 sleep，才停得下来
            left = ms
            while left > 0 and not self.stop_evt.is_set():
                chunk = min(50, left)
                dll.PP_Delay(chunk)
                left -= chunk
            self.msg_queue.put("delay: %d ms" % ms)
            return
        if t == TYPE_INPUT:
            text = str(ev.get("text", "") or "")
            n = post_text(view, text, self.stop_evt)
            self.msg_queue.put("input: 已投递 %d 个码元（%s）" % (n, text_preview(text)))
            return
        if t == TYPE_KEY:
            expr = combo_of(ev)
            action = ev.get("action", "press")
            label = key_text(expr, action)
            n = post_key(view, expr, action)
            if n == -1:
                self.msg_queue.put("key: 无法识别按键「%s」，已跳过" % expr)
            elif n == -3:
                self.msg_queue.put("key: %s 拿不到目标线程输入状态，只发了裸消息"
                                   "（组合键可能无效）" % label)
            elif n == 0:
                self.msg_queue.put("key: %s 发送失败" % label)
            else:
                self.msg_queue.put("key: %s -> %d 条消息" % (label, n))
            return
        if t == TYPE_FIND:
            image = str(ev.get("image", "") or "").encode("utf-8")
            rc = dll.PP_FindImage(view, image, float(ev.get("threshold", 0.9) or 0.9),
                                  int(ev.get("timeout_ms", 0) or 0))
            if rc == -4:
                self.msg_queue.put("findImage: 未实现，已跳过（%s）" % ev.get("image", ""))
            else:
                self.msg_queue.put("findImage: rc=%d（%s）" % (rc, ev.get("image", "")))
            return

        action = ACTION_INDEX.get(ev.get("action", "click"), 3)
        button = BUTTON_INDEX.get(ev.get("button", "left"), 0)
        px = int(round(float(ev.get("x", 0) or 0) * scale))
        py = int(round(float(ev.get("y", 0) or 0) * scale))
        rc = dll.PP_Mouse(view, px, py, button, action)
        self.msg_queue.put("mouse: %s %s (%d, %d) -> rc=%d"
                           % (ev.get("button", "left"), ev.get("action", "click"), px, py, rc))

    # -- 日志 --------------------------------------------------------------
    def _poll_messages(self) -> None:
        try:
            while True:
                msg = self.msg_queue.get_nowait()
                if msg == "__DONE__":
                    self._finish_run()
                    continue
                if isinstance(msg, str) and msg.startswith("__ERROR__"):
                    self._log(msg[len("__ERROR__"):], tag="warn")
                    self._finish_run()
                    continue
                self._log(str(msg))
        except queue.Empty:
            pass
        self.root.after(100, self._poll_messages)

    def _finish_run(self) -> None:
        self.running = False
        self.run_btn.config(state="normal")
        self.stop_btn.config(state="disabled")
        self._status("完成")

    def _log(self, text: str, tag: str | None = None) -> None:
        line = "%s  %s" % (time.strftime("%H:%M:%S"), text)
        self.log_box.configure(state="normal")
        if tag:
            self.log_box.insert("end", line + "\n", tag)
        else:
            self.log_box.insert("end", line + "\n")
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")

    def _status(self, text: str) -> None:
        self.status_var.set(text)


def main() -> None:
    enable_dpi_awareness()
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
