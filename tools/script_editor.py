"""TTVoice 点击脚本编辑器（极简版）。

独立窗口，做三件事：
  1. 在 TTVoice 界面上「拾取坐标」——点一下目标位置即记录（这次点击会被吞掉，
     不会触发应用），坐标按 FLUTTERVIEW 相对逻辑 px（96 dpi）保存；
  2. 编辑事件表（坐标 / 点击类型 / 间隔 / 滚轮 / 门控 / 粘贴内容）；
  3. 存成 JSON 供 DLL 读取执行；本窗口也能直接「试跑」验证坐标。

脚本目录：%APPDATA%\\TTVoice\\scripts\\<名称>.json
保存时还会写同目录的 active.txt（记下刚保存的脚本名），DLL 优先跑这一个；
overlay.ini 里的 script=<名称> 可以强制指定，两者都没有就跑最新的 .json。
运行：python tools/script_editor.py

JSON 格式（DLL 侧读取的就是这一份）：

    {
      "version": 1,
      "name": "活动",
      "hotkey": "",
      "loop": 1,
      "events": [
        {"x": 27, "y": 113, "click": "left", "delay_ms": 600,
         "wheel": 0, "text": "", "wait_px": 0, "wait_max_ms": 0},
        {"x": 1573, "y": 87, "click": "left", "delay_ms": 0,
         "wheel": 0, "text": "", "wait_px": 40, "wait_max_ms": 4000}
      ]
    }

字段含义：
  x, y          坐标，FLUTTERVIEW 客户区相对逻辑 px（96 dpi）；物理 px = 逻辑 * DPI/96
  click         left | double | right
  hotkey        F1..F24 / 单个字母或数字；空 = 不注册（避免抢应用自己的键）
  loop          整份脚本重复执行的轮数
  delay_ms      本行执行完之后的等待（下一行开始前）
  wheel         滚轮格数：正=向上，负=向下，0=不滚
  text          非空时先把文本写进剪贴板，再给 TTVoice 发 Ctrl+V（需 TTVoice 在前台）
  wait_px       门控：先等到 (x,y) 周围出现至少这么多「奶油色」像素再执行本行；0=不等
  wait_max_ms   门控最长等待，超时后照常执行本行（不中止）
  view_size     编辑器额外写的 [w,h]（保存时的 view 客户区），DLL 不认这个键，只用来提醒坐标基准变了

                注意：events 为空时 DLL 拒收（count>0 才算解析成功），会打
                script: parse failed <path> (no events / bad json)。硬编码的两段
                点击链已移除，所以这时点叠加层按钮不会有任何动作。
                叠加层按钮走 ScriptTryRun(true)：这类「跑不起来」的原因行
                **每次点击都会重打一次**（DLL 的 2s 轮询线程仍只打一次）。

与 DLL 的关系（很重要）：
  DLL（script_engine.inc）是最终交付的执行器；本编辑器是它的开发/调试镜像，
  所以「试跑」的每一步都与 DLL 逐字对齐：同一套消息序列、同一个门控算法
  （先睡 poll 再抓帧、连续 3 次命中、±200×±40 物理px 带内同色阈值）、同一套
  坐标换算（int(逻辑px * DPI/96) 截断）、同一批日志字样（可直接与叠加层界面
  日志/%TEMP% 日志逐行比对）。执行日志窗格可以勾「跟随 DLL 日志」把 DLL 自己写的
  行拉进来，两边放在一个窗里看。保存后还会打印「DLL 下次会加载哪份脚本」。
  只有两处故意不同：编辑器能「停止」（DLL 跑完为止），以及「门控检查（不点）」
  只看不动。其他任何行为差异都是 bug，要两边同步改。
"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import struct
import threading
import time
import tkinter as tk
from ctypes import wintypes
from tkinter import messagebox, simpledialog, ttk

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
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
PW_RENDERFULLCONTENT = 0x00000002
CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
VK_CONTROL = 0x11
VK_V = 0x56
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

MAIN_CLASS = "FLUTTER_RUNNER_WIN32_WINDOW"
VIEW_CLASS = "FLUTTERVIEW"

# 奶油色胶囊的判定（与 overlay.inc / 文档同一套阈值）
CREAM_R, CREAM_G, CREAM_B, CREAM_DIFF = 228, 210, 228, 22

CLICK_LABELS = {"left": "左键单击", "double": "左键双击", "right": "右键单击"}
LABEL_CLICKS = {v: k for k, v in CLICK_LABELS.items()}

# click 名字 -> DLL 的 kind 数字（script_engine.inc ClickFromName）
CLICK_KINDS = {"left": 0, "double": 1, "right": 2}

# --- 与 DLL 对齐的常量/上限（改一边必须改另一边）-------------------------
GATE_POLL_MS = 120        # = overlay.ini 的 gate_poll_ms（DLL 侧只认这个键）
GATE_STABLE = 3           # DLL：连续 3 次命中才放行
GATE_MAX_MS = 4000        # wait_max_ms <= 0 时 DLL 用的上限
CREAM_MIN_PX = 40         # 「门控检查（不点）」没填 wait_px 时用的默认阈值
DBLCLK_GAP_MS = 40        # 双击两条消息之间的 Sleep
PASTE_FOCUS_MS = 80       # 粘贴前 SetForegroundWindow 后的等待
MAX_EVENTS = 32           # kScriptMaxEvents：DLL 只读前 32 行
MAX_TEXT = 255            # kScriptText-1：更长的 text 会被 DLL 截断
MAX_LOOP = 99             # loop 上限

# DLL 的日志文件（LogFromOverlay 写）与 ini（script= 会覆盖 active.txt）
DLL_LOG = os.path.join(os.environ.get("TEMP", "."), "tt_version_proxy.log")
OVERLAY_INI = os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "overlay.ini")

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


class BITMAPINFOHEADER(ctypes.Structure):
    """ctypes.wintypes 没有这个结构（GDI 的 DIB 头）。"""

    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class RGBQUAD(ctypes.Structure):
    _fields_ = [
        ("rgbBlue", ctypes.c_ubyte),
        ("rgbGreen", ctypes.c_ubyte),
        ("rgbRed", ctypes.c_ubyte),
        ("rgbReserved", ctypes.c_ubyte),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", RGBQUAD * 1)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTUNION)]


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
user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
user32.OpenClipboard.argtypes = (wintypes.HWND,)
user32.EmptyClipboard.argtypes = ()
user32.SetClipboardData.argtypes = (wintypes.UINT, wintypes.HANDLE)
user32.SetClipboardData.restype = wintypes.HANDLE
user32.CloseClipboard.argtypes = ()

kernel32.GetCurrentThreadId.restype = wintypes.DWORD
kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = (
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
kernel32.GlobalAlloc.argtypes = (wintypes.UINT, ctypes.c_size_t)
kernel32.GlobalAlloc.restype = wintypes.HANDLE
kernel32.GlobalLock.argtypes = (wintypes.HANDLE,)
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = (wintypes.HANDLE,)

gdi32.CreateCompatibleDC.argtypes = (wintypes.HDC,)
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateDIBSection.argtypes = (
    wintypes.HDC, ctypes.POINTER(BITMAPINFO), wintypes.UINT,
    ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD)
gdi32.CreateDIBSection.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = (wintypes.HDC, wintypes.HGDIOBJ)
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.DeleteObject.argtypes = (wintypes.HGDIOBJ,)
gdi32.DeleteDC.argtypes = (wintypes.HDC,)


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
    """当前找到的 TTVoice 窗口（主窗口 + FLUTTERVIEW）。"""

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
            return "未找到 TTVoice（先启动应用，再点「刷新」）"
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


# --- 点击/滚轮/粘贴 -------------------------------------------------------
# 下面三个函数是 script_engine.inc SimulateClickKindAt / SimulateWheelAt /
# PasteTextToApp 的等价物：消息顺序、wParam、时间间隔都要一致。
def post_click(view: int, px: int, py: int, kind: str) -> None:
    lp = make_lparam(px, py)
    user32.PostMessageW(view, WM_MOUSEMOVE, 0, lp)
    if kind == "right":
        user32.PostMessageW(view, WM_RBUTTONDOWN, MK_RBUTTON, lp)
        user32.PostMessageW(view, WM_RBUTTONUP, 0, lp)
        return
    if kind == "double":
        user32.PostMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, lp)
        user32.PostMessageW(view, WM_LBUTTONUP, 0, lp)
        time.sleep(DBLCLK_GAP_MS / 1000.0)
        user32.PostMessageW(view, WM_LBUTTONDBLCLK, MK_LBUTTON, lp)
        user32.PostMessageW(view, WM_LBUTTONUP, 0, lp)
        return
    user32.PostMessageW(view, WM_LBUTTONDOWN, MK_LBUTTON, lp)
    user32.PostMessageW(view, WM_LBUTTONUP, 0, lp)


def post_wheel(view: int, px: int, py: int, notches: int) -> tuple[int, int]:
    """返回消息里的屏幕坐标（DLL 的日志就是打这两个数）。"""
    if not notches:
        return px, py
    pt = wintypes.POINT(px, py)
    user32.ClientToScreen(view, ctypes.byref(pt))
    delta = 120 * int(notches)
    wparam = (delta & 0xFFFF) << 16
    user32.PostMessageW(view, WM_MOUSEWHEEL, wparam, make_lparam(pt.x, pt.y))
    return pt.x, pt.y


def set_clipboard_text(text: str) -> bool:
    if not user32.OpenClipboard(None):
        return False
    try:
        user32.EmptyClipboard()
        data = text.encode("utf-16-le") + b"\x00\x00"
        handle = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
        if not handle:
            return False
        ptr = kernel32.GlobalLock(handle)
        if ptr:
            ctypes.memmove(ptr, data, len(data))
            kernel32.GlobalUnlock(handle)
        user32.SetClipboardData(CF_UNICODETEXT, handle)  # 所有权移交给系统
    finally:
        user32.CloseClipboard()
    return True


def _key(vk: int, up: bool = False) -> INPUT:
    item = INPUT()
    item.type = INPUT_KEYBOARD
    item.u.ki.wVk = vk
    item.u.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    return item


def send_ctrl_v() -> None:
    arr = (INPUT * 4)(
        _key(VK_CONTROL), _key(VK_V), _key(VK_V, up=True), _key(VK_CONTROL, up=True))
    user32.SendInput(4, arr, ctypes.sizeof(INPUT))


# --- 抓帧 + 奶油色像素计数（门控用）--------------------------------------
class ViewCapture:
    """PrintWindow(PW_RENDERFULLCONTENT) 抓 FLUTTERVIEW，只读需要的像素带。"""

    def __init__(self, view: int):
        self.view = view
        self.w = 0
        self.h = 0
        self.hdc = None
        self.bmp = None
        self.bits = None

    def _free(self) -> None:
        if self.hdc and self.bmp:
            gdi32.SelectObject(self.hdc, None)
            gdi32.DeleteObject(self.bmp)
            self.bmp = None
        if self.hdc:
            gdi32.DeleteDC(self.hdc)
            self.hdc = None
        self.bits = None

    def _ensure(self) -> bool:
        rect = wintypes.RECT()
        if not user32.GetClientRect(self.view, ctypes.byref(rect)):
            return False
        w, h = rect.right - rect.left, rect.bottom - rect.top
        if w <= 0 or h <= 0:
            return False
        if self.hdc and self.w == w and self.h == h:
            return True

        self._free()
        self.hdc = gdi32.CreateCompatibleDC(None)
        if not self.hdc:
            return False
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = w
        info.bmiHeader.biHeight = -h          # 负数 = 顶朝下，行 0 是最上面
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = 0      # BI_RGB
        bits = ctypes.c_void_p()
        self.bmp = gdi32.CreateDIBSection(self.hdc, ctypes.byref(info), 0,
                                          ctypes.byref(bits), None, 0)
        if not self.bmp or not bits.value:
            self._free()
            return False
        gdi32.SelectObject(self.hdc, self.bmp)
        self.bits = bits
        self.w, self.h = w, h
        return True

    def grab(self) -> bool:
        if not self._ensure():
            return False
        return bool(user32.PrintWindow(self.view, self.hdc, PW_RENDERFULLCONTENT))

    def count_cream(self, cx: int, cy: int, half_w: int = 200, half_h: int = 40):
        """数 (cx,cy) 周围带内奶油色像素，返回 (hits, centroid_x, centroid_y)。"""
        if not self.bits:
            return 0, 0, 0
        x0, x1 = max(0, cx - half_w), min(self.w - 1, cx + half_w)
        y0, y1 = max(0, cy - half_h), min(self.h - 1, cy + half_h)
        if x0 > x1 or y0 > y1:
            return 0, 0, 0

        stride = self.w * 4
        row_bytes = (x1 - x0 + 1) * 4
        base = self.bits.value
        hits = 0
        sum_x = 0
        sum_y = 0
        for y in range(y0, y1 + 1):
            row = ctypes.string_at(base + y * stride + x0 * 4, row_bytes)
            for i in range(0, row_bytes, 4):
                b = row[i]
                g = row[i + 1]
                r = row[i + 2]
                if r > CREAM_R and g > CREAM_G and b < CREAM_B and (r - b) > CREAM_DIFF:
                    hits += 1
                    sum_x += x0 + i // 4
                    sum_y += y
        if not hits:
            return 0, 0, 0
        return hits, sum_x // hits, sum_y // hits

    def save_bmp(self, path: str) -> bool:
        """把整帧存成 32bpp BMP，看「DLL 门控看到的是什么画面」。"""
        if not self.bits or self.w <= 0 or self.h <= 0:
            return False
        stride = self.w * 4
        try:
            with open(path, "wb") as fh:
                # BITMAPFILEHEADER：bfType/ bfSize/ 两个 WORD 保留 / bfOffBits = 14 字节
                fh.write(struct.pack("<2sIHHI", b"BM", 14 + 40 + stride * self.h,
                                     0, 0, 54))
                fh.write(struct.pack("<IiiHHIIiiII", 40, self.w, self.h, 1, 32, 0,
                                     stride * self.h, 0, 0, 0, 0))
                base = self.bits.value
                # DIB 是顶朝下，BMP 是底朝下，所以逐行逆序写
                for y in range(self.h - 1, -1, -1):
                    fh.write(ctypes.string_at(base + y * stride, stride))
        except OSError:
            return False
        return True


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
    FIELDS = ("x", "y", "click", "delay_ms", "wheel", "text", "wait_px", "wait_max_ms")

    def __init__(self, master, event: dict, on_pick, on_cancel=None):
        super().__init__(master)
        self.title("事件")
        self.resizable(False, False)
        self.result = None
        self._on_pick = on_pick
        self._on_cancel = on_cancel

        self.x_var = tk.StringVar(value=str(event.get("x", 0)))
        self.y_var = tk.StringVar(value=str(event.get("y", 0)))
        self.click_var = tk.StringVar(value=CLICK_LABELS.get(event.get("click", "left"), "左键单击"))
        self.delay_var = tk.StringVar(value=str(event.get("delay_ms", 0)))
        self.wheel_var = tk.StringVar(value=str(event.get("wheel", 0)))
        self.text_var = tk.StringVar(value=event.get("text", "") or "")
        self.wait_px_var = tk.StringVar(value=str(event.get("wait_px", 0)))
        self.wait_max_var = tk.StringVar(value=str(event.get("wait_max_ms", 4000)))

        body = ttk.Frame(self, padding=10)
        body.grid(row=0, column=0, sticky="nsew")

        rows = [
            ("坐标 X（逻辑 px）", ttk.Entry(body, textvariable=self.x_var, width=12)),
            ("坐标 Y（逻辑 px）", ttk.Entry(body, textvariable=self.y_var, width=12)),
            ("点击事件", ttk.Combobox(body, textvariable=self.click_var, width=12,
                                      state="readonly", values=list(CLICK_LABELS.values()))),
            ("间隔时间 ms（点完后等）", ttk.Entry(body, textvariable=self.delay_var, width=12)),
            ("滚轮格数（负=向下）", ttk.Entry(body, textvariable=self.wheel_var, width=12)),
            ("粘贴内容（需前台）", ttk.Entry(body, textvariable=self.text_var, width=32)),
            ("门控：等到 ≥N px 奶油色", ttk.Entry(body, textvariable=self.wait_px_var, width=12)),
            ("门控上限 ms", ttk.Entry(body, textvariable=self.wait_max_var, width=12)),
        ]
        for i, (label, widget) in enumerate(rows):
            ttk.Label(body, text=label).grid(row=i, column=0, sticky="w", pady=3, padx=(0, 8))
            widget.grid(row=i, column=1, sticky="w", pady=3)

        buttons = ttk.Frame(body)
        buttons.grid(row=len(rows), column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(buttons, text="拾取坐标", command=self._pick).pack(side="left", padx=4)
        ttk.Button(buttons, text="确定", command=self._ok).pack(side="left", padx=4)
        ttk.Button(buttons, text="取消", command=self.destroy).pack(side="left")

        self.transient(master)
        self.grab_set()
        self.protocol("WM_DELETE_WINDOW", self.destroy)

    def _pick(self):
        def fill(lx, ly):
            if not self.winfo_exists():
                return
            self.x_var.set(str(lx))
            self.y_var.set(str(ly))
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
                "text": self.text_var.get(),
                "wait_px": int(float(self.wait_px_var.get() or 0)),
                "wait_max_ms": int(float(self.wait_max_var.get() or 0)),
            }
        except ValueError:
            messagebox.showerror("格式错误", "坐标/间隔/滚轮/门控必须是数字", parent=self)
            return
        if event["x"].is_integer():
            event["x"] = int(event["x"])
        if event["y"].is_integer():
            event["y"] = int(event["y"])
        self.result = event
        self.destroy()


# --- 主窗口 ---------------------------------------------------------------
DEFAULT_EVENTS = [
    {"x": 27, "y": 113, "click": "left", "delay_ms": 600,
     "wheel": 0, "text": "", "wait_px": 0, "wait_max_ms": 0},
    {"x": 1573, "y": 87, "click": "left", "delay_ms": 0,
     "wheel": 0, "text": "", "wait_px": 40, "wait_max_ms": 4000},
]


class App:
    COLUMNS = (
        ("idx", "序号", 50, "center"),
        ("pos", "坐标", 130, "center"),
        ("click", "点击", 90, "center"),
        ("delay", "间隔ms", 80, "center"),
        ("wheel", "滚轮", 70, "center"),
        ("gate", "门控", 120, "center"),
        ("text", "粘贴内容", 200, "w"),
    )

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("TTVoice 点击脚本编辑器")
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
        self.status_var = tk.StringVar(value="")

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
        ttk.Label(meta, text="（空 = 不注册；F1~F24 / 单字母）", foreground="#777").pack(side="left")

        tools = ttk.Frame(outer)
        tools.pack(fill="x", pady=(0, 6))
        self.pick_btn = ttk.Button(tools, text="添加坐标（拾取）", command=self.add_by_pick)
        self.pick_btn.pack(side="left")
        ttk.Button(tools, text="添加事件", command=self.add_manual).pack(side="left", padx=(4, 0))
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
        ttk.Button(bottom, text="门控检查（不点）", command=self.do_probe).pack(
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
                             stretch=(key == "text"))
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
            messagebox.showwarning("找不到 TTVoice",
                                   "没找到 FLUTTERVIEW 子窗口。先启动 TT语音，再点「刷新目标」。")
            return False
        return True

    # -- 表格 --------------------------------------------------------------
    def render_table(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, ev in enumerate(self.events, 1):
            gate = ""
            if int(ev.get("wait_px") or 0) > 0:
                gate = "≥%dpx/%dms" % (int(ev["wait_px"]), int(ev.get("wait_max_ms") or 0))
            self.tree.insert("", "end", iid=str(i - 1), values=(
                i,
                "%s, %s" % (self._num(ev.get("x", 0)), self._num(ev.get("y", 0))),
                CLICK_LABELS.get(ev.get("click", "left"), "左键单击"),
                ev.get("delay_ms", 0),
                ev.get("wheel", 0),
                gate or "-",
                ev.get("text", "") or "",
            ))

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

    def add_by_pick(self) -> None:
        if self.picker is not None:
            self._end_pick()
            self._status("已取消拾取")
            return
        if not self._ensure_target():
            return

        def on_point(lx, ly):
            ev = dict(DEFAULT_EVENTS[0])
            ev.update(x=lx, y=ly, delay_ms=0, wait_px=0, wait_max_ms=0)
            dialog = EventDialog(self.root, ev, self.start_pick, self._end_pick)
            self.root.wait_window(dialog)
            if dialog.result:
                self._add_event(dialog.result)

        self.start_pick(on_point)

    def add_manual(self) -> None:
        dialog = EventDialog(self.root, dict(DEFAULT_EVENTS[0]), self.start_pick, self._end_pick)
        self.root.wait_window(dialog)
        if dialog.result:
            self._add_event(dialog.result)

    def edit_selected(self) -> None:
        idx = self._selected_index()
        if idx is None:
            self._status("先选中一行")
            return
        dialog = EventDialog(self.root, self.events[idx], self.start_pick, self._end_pick)
        self.root.wait_window(dialog)
        if dialog.result:
            self.events[idx] = dialog.result
            self.dirty = True
            self.render_table()
            self.tree.selection_set(str(idx))

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
        self.pick_btn.config(text="取消拾取")
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
        try:
            self.pick_btn.config(text="添加坐标（拾取）")
        except tk.TclError:
            pass

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
        size = data.get("view_size") or []
        self.script_view_size = (int(size[0]), int(size[1])) if len(size) == 2 else None
        self.dirty = False
        self.script_combo.set(name)
        self.render_table()
        self._status("已载入 %s（%d 行）" % (path, len(self.events)))

    def _collect(self) -> dict:
        w, h = self.target.w, self.target.h
        data = {
            "version": 1,
            "name": self.name_var.get().strip() or self.current_name or "script",
            "hotkey": self.hotkey_var.get().strip(),
            "loop": min(MAX_LOOP, max(1, self._int(self.loop_var.get(), 1))),
            "events": self.events,
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
        self._status("已保存 %s" % path)
        self._warn_dll_limits()
        self._log_dll_view()

    def _write_active(self, name: str) -> None:
        """DLL 按这个文件选脚本；overlay.ini 的 script= 优先于它。"""
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
        for i, ev in enumerate(self.events, 1):
            text = str(ev.get("text") or "")
            if len(text) > MAX_TEXT:
                self._log("parity: 第 %d 行 text %d 字符 > %d，DLL 会截断" % (i, len(text), MAX_TEXT))

    def _dll_will_run(self) -> tuple[str, str]:
        """按 DLL 的优先级算它下一次会加载哪份：ini script= > active.txt > 最新 .json。"""
        forced = ""
        try:
            with open(OVERLAY_INI, "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if line.lower().startswith("script="):
                        forced = line.split("=", 1)[1].strip()
        except OSError:
            pass
        if forced:
            if forced.lower().endswith(".json"):
                forced = forced[:-5]
            return forced, "overlay.ini script="

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
            # ini script= 指空了就直接当「没脚本」，不再往下看 active.txt / 最新 json
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
            self._log("警告: DLL 加载的不是当前脚本「%s」——检查 overlay.ini 的 script= 或 active.txt"
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

    def do_probe(self) -> None:
        """门控检查：按 DLL 的算法走一遍等待，但一个消息都不发。"""
        idx = self._selected_index()
        if idx is None:
            self._status("先选中一行")
            return
        self._start_job([dict(self.events[idx])], 1, idx + 1, "probe")

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
        self.run_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        threading.Thread(target=self._job_thread,
                         args=(events, loop, first_index, mode,
                               self.name_var.get().strip() or self.current_name or "script",
                               self.target.main, self.target.view, self.target.scale,
                               self.target.w, self.target.h),
                         daemon=True).start()

    def do_stop(self) -> None:
        self.stop_evt.set()
        self._status("正在停止…")

    def _job_thread(self, events, loop, first_index, mode, name,
                    main_hwnd, view, scale, view_w, view_h) -> None:
        cap = ViewCapture(view)
        started = time.time()
        try:
            self.msg_queue.put("target: view %dx%d scale %.2f" % (view_w, view_h, scale))
            self._check_view_size(view_w, view_h)
            if mode == "probe":
                self._probe_once(cap, events[0], first_index, scale)
            else:
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
                        self._run_event(cap, ev, main_hwnd, view, scale, last)
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
            cap._free()

    def _run_event(self, cap, ev, main_hwnd, view, scale, skip_delay) -> None:
        lx = float(ev.get("x", 0) or 0)
        ly = float(ev.get("y", 0) or 0)
        # DLL: static_cast<LONG>(lx * g_scale) —— 截断不是四舍五入，这里保持一致
        px, py = int(lx * scale), int(ly * scale)

        wait_px = int(ev.get("wait_px") or 0)
        if wait_px > 0:
            self._gate_wait(cap, px, py, wait_px, int(ev.get("wait_max_ms") or 0))

        wheel = int(ev.get("wheel") or 0)
        if wheel:
            sx, sy = post_wheel(view, px, py, wheel)
            self.msg_queue.put("script: wheel %+d at %d,%d" % (wheel, sx, sy))

        text = ev.get("text") or ""
        if text:
            if len(str(text)) > MAX_TEXT:
                self.msg_queue.put("parity: text %d 字符 > %d，DLL 只能粘前 %d 个"
                                   % (len(str(text)), MAX_TEXT, MAX_TEXT))
            if set_clipboard_text(str(text)):
                user32.SetForegroundWindow(main_hwnd)
                time.sleep(PASTE_FOCUS_MS / 1000.0)
                send_ctrl_v()
                self.msg_queue.put('script: pasted "%s"' % text)
            else:
                self.msg_queue.put("script: clipboard busy, paste skipped")

        kind = ev.get("click", "left")
        post_click(view, px, py, kind)
        self.msg_queue.put("script: click kind=%d %d,%d"
                           % (CLICK_KINDS.get(kind, 0), px, py))

        delay = max(0, int(ev.get("delay_ms") or 0))
        if delay and not skip_delay:
            self.stop_evt.wait(delay / 1000.0)

    def _gate_wait(self, cap, px, py, wait_px, wait_max_ms) -> bool:
        """WaitForCreamAt 的翻版：先睡 poll 再抓帧，连续 3 次命中才放行。"""
        limit = wait_max_ms if wait_max_ms > 0 else GATE_MAX_MS
        start = time.time()
        stable = 0
        max_hits = 0
        while (time.time() - start) * 1000.0 < limit:
            if self.stop_evt.is_set():
                return False
            time.sleep(GATE_POLL_MS / 1000.0)
            if not cap.grab():
                continue
            hits, bx, by = cap.count_cream(px, py)
            if hits > max_hits:
                max_hits = hits
            if hits >= wait_px:
                stable += 1
                if stable >= GATE_STABLE:
                    self.msg_queue.put("script: gate hit %dpx at %d,%d after %dms"
                                       % (hits, bx, by, int((time.time() - start) * 1000)))
                    return True
            else:
                stable = 0
        self.msg_queue.put("script: gate timeout %dms (max %dpx)" % (limit, max_hits))
        return False

    def _probe_once(self, cap, ev, index, scale) -> None:
        wait_px = int(ev.get("wait_px") or 0)
        need = wait_px if wait_px > 0 else CREAM_MIN_PX
        px, py = int(float(ev.get("x", 0) or 0) * scale), int(float(ev.get("y", 0) or 0) * scale)
        self.msg_queue.put("probe: 行%d 物理 %d,%d need=%dpx（不发任何点击）"
                           % (index, px, py, need))
        hit = self._gate_wait(cap, px, py, need, int(ev.get("wait_max_ms") or 0))
        path = os.path.join(os.environ.get("TEMP", "."),
                            "ttvoice_probe_line%d.bmp" % index)
        if cap.save_bmp(path):
            self.msg_queue.put("probe: 抓帧已存 %s" % path)
        if hit:
            self.msg_queue.put("probe: 行%d 门控会命中 -> DLL 一到就点" % index)
        else:
            self.msg_queue.put("probe: 行%d 门控不命中 -> DLL 等满后照点（脚本没有 force 开关）" % index)

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
