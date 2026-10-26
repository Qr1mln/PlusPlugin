r"""宿主应用窗口查找：定位 FLUTTER_RUNNER_WIN32_WINDOW 主窗口及其 FLUTTERVIEW 子窗口。"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

from .win32 import (MAIN_CLASS, VIEW_CLASS, WNDENUMPROC, get_class, proc_name,
                    user32)


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