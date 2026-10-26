r"""脚本编辑器主窗口与程序入口。"""
from __future__ import annotations

import json
import os
import queue
import time
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk

from .dialogs import EventDialog
from .engine_client import (ENGINE_HOST, ENGINE_PORT, EngineClient, EngineSession,
                            MSG_DONE, MSG_ERROR)
from .events import (MAX_EVENTS, MAX_LOOP, TYPE_DELAY, TYPE_FIND, TYPE_INPUT, TYPE_KEY,
                     TYPE_LABELS, TYPE_MOUSE, clean_event, default_event, event_detail)
from .picker import ClickPicker, PickOverlay
from .target import Target, find_target
from .win32 import enable_dpi_awareness


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

        self.session = EngineSession(EngineClient())
        self.name_var = tk.StringVar(value="活动")
        self.loop_var = tk.StringVar(value="1")
        self.status_var = tk.StringVar(value="")

        self._build()
        self.refresh_target(quiet=True)
        self._status(self.target.describe())
        self.refresh_script_list(load_first=True)
        self.render_table()
        self._log("sock: 目标 %s:%d（执行由应用进程内的 script-engine 完成）"
                  % (ENGINE_HOST, ENGINE_PORT))
        self.root.after(100, self._poll_messages)

    def open_schedule(self) -> None:
        """打开定时任务管理窗口（单实例，已开则提到前面并刷新状态）。"""
        from .schedule import ScheduleDialog

        dlg = getattr(self, "schedule_dlg", None)
        if dlg is not None and dlg.winfo_exists():
            dlg.lift()
            dlg.refresh_status()
            return
        names = [str(v) for v in (self.script_combo["values"] or ())]
        self.schedule_dlg = ScheduleDialog(self.root, names, self._log,
                                           scripts_dir=self.scripts_dir)

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
        ttk.Button(files, text="定时任务", width=8, command=self.open_schedule).pack(
            side="left", padx=(8, 0))

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
    def render_table(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for i, ev in enumerate(self.events, 1):
            self.tree.insert("", "end", iid=str(i - 1),
                             tags=(self.tree_tag_odd if i % 2 else self.tree_tag_even,),
                             values=(i, TYPE_LABELS.get(ev.get("type", TYPE_MOUSE), "鼠标事件"),
                                     event_detail(ev), ev.get("remark", "") or ""))
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
        dialog = EventDialog(self.root, default_event(type_key), self._begin_pick, title="添加事件")
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
        self.events = [clean_event(e) for e in data.get("events", []) if isinstance(e, dict)]
        self.dirty = False
        self.script_combo.set(name)
        self.render_table()
        self._status("已载入 %s（%d 行）" % (name, len(self.events)))
        self._log("file: %s" % path)

    def _collect(self) -> dict:
        return {
            "version": 2,
            "name": self.name_var.get().strip() or self.current_name or "script",
            "loop": min(MAX_LOOP, max(1, self._int(self.loop_var.get(), 1))),
            "events": [clean_event(e) for e in self.events],
        }

    @staticmethod
    def _int(text: str, default: int) -> int:
        try:
            return int(float(text))
        except (TypeError, ValueError):
            return default

    def new_script(self) -> None:
        if self.session.running:
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
        if self.session.running:
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

    # -- 执行（推送脚本到宿主进程内的 script-engine，由 DLL 在进程内执行）----
    def _ensure_engine(self) -> bool:
        if not self.session.connect(2.0):
            messagebox.showerror(
                "连接失败",
                "连不上引擎端口 %s:%d\n请确认应用已启动、且 script-engine 插件已加载（enabled=true）。"
                % (ENGINE_HOST, ENGINE_PORT))
            return False
        return True

    def _begin_run(self, events: list[dict], loop: int) -> None:
        if self.session.running:
            self._log("run ignored, previous run still active")
            return
        if not events:
            messagebox.showinfo("提示", "事件表是空的")
            return
        if not self._ensure_engine():
            return
        loop = min(MAX_LOOP, max(1, loop))
        self.run_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self._log("sock: 已发送脚本（%d 行, loop=%d），应用进程内执行" % (len(events), loop))
        self.session.start(events, loop)

    def do_run(self) -> None:
        self._begin_run([clean_event(e) for e in self.events],
                        self._int(self.loop_var.get(), 1))

    def do_run_selected(self) -> None:
        idx = self._selected_index()
        if idx is None or idx >= len(self.events):
            self._status("先选中一行")
            return
        self._begin_run([clean_event(self.events[idx])], 1)

    def do_stop(self) -> None:
        if not self.session.running:
            return
        self.session.stop()
        self._status("正在停止…")

    # -- 日志 --------------------------------------------------------------
    def _poll_messages(self) -> None:
        while True:
            item = self.session.drain()
            if item is None:
                break
            kind, text = item
            if kind == MSG_DONE:
                self._finish_run()
            elif kind == MSG_ERROR:
                self._log(text, tag="warn")
                self._finish_run()
            else:
                self._log(text)
        # 看门狗：执行超过 RUN_TIMEOUT 仍无 done（应用/DLL 可能已卡死），强制收尾并断连，
        # 让运行按钮恢复可用，且下次运行重新连接，而不是卡在写满的连接上。
        if self.session.timed_out():
            self._log("sock: 执行超时（应用可能无响应），强制结束并断开", tag="warn")
            self._finish_run()
            self.session.close()
        self.root.after(100, self._poll_messages)

    def _finish_run(self) -> None:
        self.session.finish()
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