r"""事件编辑对话框：按事件类型切换鼠标/延迟/找图/输入/按键各面板。"""
from __future__ import annotations

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from .events import (DELAY_UNITS, KEY_ACTIONS, KEY_CHOICES, LABEL_ACTIONS, LABEL_BUTTONS,
                     LABEL_KEY_ACTIONS, LABEL_TYPES, MOUSE_ACTIONS, MOUSE_BUTTONS,
                     TYPE_DELAY, TYPE_FIND, TYPE_INPUT, TYPE_KEY, TYPE_LABELS, TYPE_MOUSE,
                     combo_of, fmt_coord, key_text, normalize_combo, resolve_vk, split_combo)


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
        self.x_var = tk.StringVar(value=fmt_coord(ev.get("x", 0)))
        self.y_var = tk.StringVar(value=fmt_coord(ev.get("y", 0)))
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
                  text="逐字符后台投递到目标窗口；多行按回车发送，中英文均可").grid(
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
            self.x_var.set(fmt_coord(lx))
            self.y_var.set(fmt_coord(ly))
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