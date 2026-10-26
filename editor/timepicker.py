r"""日期 + 时分选择器（纯 tkinter，无第三方依赖）。

用法：
    picked = DateTimePicker(parent, "2026-10-27 21:30").result
    # picked 为 "YYYY-MM-DD HH:MM"，取消时为 None
"""
from __future__ import annotations

import calendar
import time
import tkinter as tk
from tkinter import ttk

WEEK_LABELS = ("一", "二", "三", "四", "五", "六", "日")
FMT = "%Y-%m-%d %H:%M"


def parse_datetime(text: str):
    """解析 "YYYY-MM-DD HH:MM"；失败返回当前本地时间（结构体）。"""
    try:
        return time.strptime(text.strip(), FMT)
    except (ValueError, AttributeError):
        return time.localtime()


def format_datetime(tm) -> str:
    return time.strftime(FMT, tm)


def center_window(win: tk.Misc, parent: tk.Misc | None = None,
                  width: int | None = None, height: int | None = None) -> None:
    """把窗口摆到父窗口（或屏幕）中央，并夹在屏幕范围内。

    可以对着还没显示（withdrawn）的窗口调用：此时尺寸用 reqwidth/reqheight，
    边框/标题栏偏移借用父窗口的——这样窗口能"一出现就在正确位置"，不会闪。
    """
    win.update_idletasks()
    w = width or win.winfo_width()
    h = height or win.winfo_height()
    if w <= 1:
        w = win.winfo_reqwidth()
    if h <= 1:
        h = win.winfo_reqheight()

    screen_w, screen_h = win.winfo_screenwidth(), win.winfo_screenheight()
    parent_shown = False
    if parent is not None:
        try:
            parent_shown = parent.winfo_width() > 1 and parent.winfo_height() > 1
        except tk.TclError:
            parent_shown = False

    if parent_shown:
        px, py = parent.winfo_rootx(), parent.winfo_rooty()
        pw, ph = parent.winfo_width(), parent.winfo_height()
    else:
        px = py = 0
        pw, ph = screen_w, screen_h

    # 客户区中心
    cx = px + pw // 2
    cy = py + ph // 2
    x = cx - w // 2
    y = cy - h // 2

    # geometry("+x+y") 定位的是带边框的窗口框架，而 winfo_rootx/rooty 是客户区。
    # 已经显示的窗口直接量自己；还没显示的借用父窗口的装饰偏移（同一样式）。
    dx = dy = 0
    ref = None
    try:
        if win.winfo_width() > 1 and win.winfo_height() > 1:
            ref = win
        elif parent_shown:
            ref = parent
    except tk.TclError:
        ref = None
    if ref is not None:
        dx = ref.winfo_rootx() - ref.winfo_x()
        dy = ref.winfo_rooty() - ref.winfo_y()

    x = max(0, min(x - dx, screen_w - w))
    y = max(0, min(y - dy, screen_h - h))
    win.geometry("+%d+%d" % (x, y))


class DateTimePicker(tk.Toplevel):
    """弹出式日历 + 时分选择；确认后 result 是格式化字符串。"""

    def __init__(self, master: tk.Misc, initial: str = ""):
        super().__init__(master)
        self.title("选择时间")
        self.resizable(False, False)
        self.transient(master)

        tm = parse_datetime(initial)
        self.year = tm.tm_year
        self.month = tm.tm_mon
        self.day = tm.tm_mday
        self.hour = tm.tm_hour
        self.minute = tm.tm_min
        self.result: str | None = None

        self.withdraw()          # 先隐藏：摆好位置再显示，避免闪一下再跳过去
        self._build()
        self._refresh()
        center_window(self, master)
        self.deiconify()
        self.grab_set()
        self.focus_set()
        self.wait_window(self)

    # -- UI ---------------------------------------------------------------
    def _build(self) -> None:
        outer = ttk.Frame(self, padding=8)
        outer.pack()

        head = ttk.Frame(outer)
        head.pack(fill="x")
        ttk.Button(head, text="◀", width=3, command=lambda: self._shift_month(-1)).pack(side="left")
        self.title_var = tk.StringVar()
        ttk.Label(head, textvariable=self.title_var, anchor="center",
                  font=("Microsoft YaHei UI", 10, "bold")).pack(side="left", expand=True, fill="x")
        ttk.Button(head, text="▶", width=3, command=lambda: self._shift_month(1)).pack(side="left")

        week = ttk.Frame(outer)
        week.pack(fill="x", pady=(6, 0))
        for i, label in enumerate(WEEK_LABELS):
            ttk.Label(week, text=label, width=4, anchor="center").grid(row=0, column=i,
                                                                       padx=1, pady=1)

        self.grid_frame = ttk.Frame(outer)
        self.grid_frame.pack(pady=(2, 6))

        bottom = ttk.Frame(outer)
        bottom.pack(fill="x", pady=(4, 0))
        ttk.Label(bottom, text="时间").pack(side="left")
        self.hour_var = tk.StringVar(value="%02d" % self.hour)
        self.minute_var = tk.StringVar(value="%02d" % self.minute)
        tk.Spinbox(bottom, from_=0, to=23, width=4, textvariable=self.hour_var,
                   wrap=True, format="%02.0f").pack(side="left", padx=(4, 0))
        ttk.Label(bottom, text=":").pack(side="left")
        tk.Spinbox(bottom, from_=0, to=59, width=4, textvariable=self.minute_var,
                   wrap=True, format="%02.0f").pack(side="left", padx=(0, 6))
        ttk.Button(bottom, text="此刻", width=6, command=self._set_now).pack(side="left")

        buttons = ttk.Frame(outer)
        buttons.pack(fill="x", pady=(8, 0))
        ttk.Button(buttons, text="取消", width=8, command=self._cancel).pack(side="right")
        ttk.Button(buttons, text="确定", width=8, command=self._confirm).pack(side="right",
                                                                             padx=(0, 6))

        self.bind("<Escape>", lambda _e: self._cancel())
        self.bind("<Return>", lambda _e: self._confirm())

    def _refresh(self) -> None:
        self.title_var.set("%d 年 %d 月" % (self.year, self.month))
        for child in self.grid_frame.winfo_children():
            child.destroy()

        today = time.localtime()
        for week_idx, week in enumerate(calendar.monthcalendar(self.year, self.month)):
            for day_idx, day in enumerate(week):
                if day == 0:
                    ttk.Label(self.grid_frame, text="", width=4).grid(
                        row=week_idx, column=day_idx, padx=1, pady=1)
                    continue
                selected = (day == self.day)
                is_today = (today.tm_year == self.year and today.tm_mon == self.month
                            and today.tm_mday == day)
                style = {"width": 4}
                btn = tk.Button(self.grid_frame, text=str(day), **style,
                                relief="solid" if selected else "flat",
                                bd=1,
                                bg="#dcebff" if selected else ("#f0f0f0" if is_today else "white"),
                                activebackground="#c8ddff",
                                command=lambda d=day: self._pick_day(d))
                btn.grid(row=week_idx, column=day_idx, padx=1, pady=1)

    def _shift_month(self, delta: int) -> None:
        month = self.month + delta
        year = self.year
        if month < 1:
            month = 12
            year -= 1
        elif month > 12:
            month = 1
            year += 1
        self.year, self.month = year, month
        # 目标月没有这一天时收敛到最后一天
        last = calendar.monthrange(self.year, self.month)[1]
        self.day = min(self.day, last)
        self._refresh()

    def _pick_day(self, day: int) -> None:
        self.day = day
        self._refresh()

    def _set_now(self) -> None:
        now = time.localtime()
        self.year, self.month, self.day = now.tm_year, now.tm_mon, now.tm_mday
        self.hour_var.set("%02d" % now.tm_hour)
        self.minute_var.set("%02d" % now.tm_min)
        self._refresh()

    def _confirm(self) -> None:
        try:
            hour = min(23, max(0, int(self.hour_var.get())))
            minute = min(59, max(0, int(self.minute_var.get())))
        except ValueError:
            hour, minute = self.hour, self.minute
        self.result = "%04d-%02d-%02d %02d:%02d" % (self.year, self.month, self.day,
                                                    hour, minute)
        self.destroy()

    def _cancel(self) -> None:
        self.result = None
        self.destroy()
