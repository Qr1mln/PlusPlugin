r"""坐标拾取：全局低级鼠标钩子（吞掉一次点击）+ 覆盖在目标 view 上的半透明遮罩。"""
from __future__ import annotations

import ctypes
import queue
import threading
import tkinter as tk
from ctypes import wintypes

from .events import fmt_coord
from .win32 import (GWL_EXSTYLE, HOOKPROC, HWND_TOPMOST, MSLLHOOKSTRUCT, SWP_NOACTIVATE,
                    SWP_NOMOVE, SWP_NOSIZE, WH_MOUSE_LL, WM_LBUTTONDOWN, WM_LBUTTONUP,
                    WM_QUIT, WM_RBUTTONDOWN, WM_RBUTTONUP, WS_EX_NOACTIVATE,
                    WS_EX_TOOLWINDOW, WS_EX_TRANSPARENT, kernel32, user32)


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