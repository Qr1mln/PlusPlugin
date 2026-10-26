r"""定时任务：配置编辑 + 与进程内调度器交互。

配置文件：%APPDATA%\TTVoice\schedule.json（DLL 只读它，编辑器负责编辑）

    {"version": 1, "jobs": [
      {"id": "j1", "name": "每天上麦", "scripts": ["进房间", "上麦", "退出房间"],
       "enabled": true, "loop": 1,
       "when": {"type": "daily", "at": "21:30", "days": [1,2,3,4,5]}},
      {"id": "j2", "name": "每30分钟", "scripts": ["xxx"], "enabled": true,
       "when": {"type": "interval", "minutes": 30}},
      {"id": "j3", "name": "一次性", "scripts": ["xxx"], "enabled": true,
       "when": {"type": "once", "at": "2026-10-27 21:30"}}]}

一个任务可以带多个脚本：`scripts` 按数组顺序依次执行（前一个跑完才开始下一个）。
早期单脚本字段 `script` 仍能读入（自动转成 `scripts`），保存时统一写成 `scripts`。

语义（DLL 侧实现）：脚本正在跑时到点排队；不补跑；days 1=周一..7=周日，
空表示每天；时间按本地时间。
"""
from __future__ import annotations

import json
import os
import time
import tkinter as tk
import uuid
from tkinter import messagebox, ttk

from .engine_client import EngineClient
from .timepicker import DateTimePicker, center_window

DAY_LABELS = (("1", "一"), ("2", "二"), ("3", "三"), ("4", "四"),
              ("5", "五"), ("6", "六"), ("7", "日"))
TYPE_LABELS = {"daily": "每天/每周", "interval": "固定间隔", "once": "一次性"}
MAX_JOBS = 64
MAX_SCRIPTS = 20


def schedule_path() -> str:
    return os.path.join(os.environ.get("APPDATA", ""), "TTVoice", "schedule.json")


def default_job(script: str = "") -> dict:
    return {
        "id": uuid.uuid4().hex[:8],
        "name": "新任务",
        "scripts": [script] if script else [],
        "enabled": True,
        "loop": 1,
        "when": {"type": "daily", "at": "21:30", "days": []},
    }


def job_scripts(job: dict) -> list[str]:
    """取任务的有序脚本列表；兼容早期的单脚本字段 script。"""
    raw = job.get("scripts")
    if isinstance(raw, list):
        scripts = [str(s) for s in raw if isinstance(s, str) and s.strip()]
        if scripts:
            return scripts
    single = job.get("script")
    if isinstance(single, str) and single.strip():
        return [single]
    return []


def describe_when(job: dict) -> str:
    """把 when 描述成一行人话；UI 与日志共用。"""
    when = job.get("when") or {}
    kind = when.get("type", "daily")
    if kind == "interval":
        return "每 %d 分钟" % int(when.get("minutes", 30) or 30)
    if kind == "once":
        return "一次性 %s" % (when.get("at", "-") or "-")
    at = when.get("at", "00:00") or "00:00"
    days = [str(d) for d in (when.get("days") or [])]
    if not days:
        return "每天 %s" % at
    labels = {k: v for k, v in DAY_LABELS}
    return "周%s %s" % ("".join(labels.get(d, d) for d in days), at)


def load_jobs() -> list[dict]:
    path = schedule_path()
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return []
    jobs = data.get("jobs") if isinstance(data, dict) else None
    if not isinstance(jobs, list):
        return []
    out: list[dict] = []
    for job in jobs:
        if not isinstance(job, dict):
            continue
        normalized = dict(job)
        normalized["scripts"] = job_scripts(job)          # 迁移 script -> scripts
        normalized.pop("script", None)
        normalized["dir"] = str(normalized.get("dir", "") or "")
        try:
            normalized["gap_ms"] = max(0, int(normalized.get("gap_ms", 0) or 0))
        except (TypeError, ValueError):
            normalized["gap_ms"] = 0
        if not isinstance(normalized.get("when"), dict):
            normalized["when"] = {"type": "daily", "at": "21:30", "days": []}
        out.append(normalized)
    return out


def save_jobs(jobs: list[dict]) -> bool:
    path = schedule_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    payload = {"version": 1, "jobs": jobs}
    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=2)
            fh.write("\n")
        os.replace(tmp, path)   # 原子替换：DLL 读到的永远是完整文件
        return True
    except OSError:
        return False


def engine_call(cmd: dict, want: str = "schedule", timeout: float = 3.0):
    """短连接调一次引擎命令，返回第一个 t==want 的帧（跳过 log 帧）。"""
    client = EngineClient()
    try:
        if not client.connect(timeout=1.5):
            return None
        try:
            client.sock.settimeout(timeout)
        except OSError:
            return None
        if not client.send(cmd):
            return None
        for _ in range(20):
            frame = client.recv_frame()
            if frame is None:
                return None
            try:
                obj = json.loads(frame.decode("utf-8"))
            except ValueError:
                continue
            if obj.get("t") == want:
                return obj
        return None
    except OSError:
        return None
    finally:
        client.close()


class ScheduleDialog(tk.Toplevel):
    """定时任务管理窗口：左边列表、右边表单。"""

    COLUMNS = (
        ("enabled", "启用", 50, "center"),
        ("name", "名称", 180, "w"),
        ("when", "触发", 240, "w"),
        ("next", "下次运行", 180, "w"),
    )

    def __init__(self, master: tk.Misc, script_names: list[str], log=None,
                 scripts_dir: str = ""):
        super().__init__(master)
        self.withdraw()   # 先不显示：布局、定尺寸、居中一次做完再 deiconify
        self.title("定时任务")
        self.log = log or (lambda *_: None)
        self.script_names = list(script_names)
        self.scripts_root = scripts_dir or os.path.join(
            os.environ.get("APPDATA", ""), "TTVoice", "scripts")
        self.jobs = load_jobs()
        self.next_map: dict[str, str] = {}
        self._selected = -1
        self.scripts: list[str] = []      # 表单里的脚本序列

        self.transient(master)
        try:
            scale = max(1.0, float(self.winfo_fpixels("1i")) / 96.0)
        except tk.TclError:
            scale = 1.0
        self.minsize(int(680 * scale), int(560 * scale))

        self._build()
        self._refresh_tree()
        self.refresh_status()

        # 高度按内容自适应：列表不会被拉成一大片空白（用户可手动拉大）。
        self.update_idletasks()
        width = max(int(760 * scale), self.winfo_reqwidth())
        height = self.winfo_reqheight()
        self.geometry("%dx%d" % (width, height))
        center_window(self, master, width, height)
        self.deiconify()   # 尺寸/位置都已定好，一次性出现（不会先闪再跳）

    # -- UI ---------------------------------------------------------------
    def _build(self) -> None:
        outer = ttk.Frame(self, padding=8)
        outer.pack(fill="both", expand=True)

        # 纵向分栏：上面设置、下面列表（列表占满剩余高度，列更宽好读）
        paned = ttk.Panedwindow(outer, orient="vertical")
        paned.pack(fill="both", expand=True)

        left = ttk.LabelFrame(paned, text="任务列表", padding=(6, 4))
        self.tree = ttk.Treeview(left, columns=[c[0] for c in self.COLUMNS],
                                 show="headings", height=6, selectmode="browse")
        for key, title, width, anchor in self.COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor=anchor,
                             stretch=(key in ("name", "when")))
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        right = ttk.LabelFrame(paned, text="任务设置", padding=(8, 6))
        paned.add(right, weight=0)    # 设置在上：按内容高度
        paned.add(left, weight=1)     # 列表在下：吃掉剩余空间
        right.columnconfigure(1, weight=1)

        self.name_var = tk.StringVar()
        self.dir_var = tk.StringVar()
        self.pick_var = tk.StringVar()
        self.type_var = tk.StringVar(value=TYPE_LABELS["daily"])
        self.at_var = tk.StringVar(value="21:30")
        self.minutes_var = tk.StringVar(value="30")
        self.once_var = tk.StringVar(value=time.strftime("%Y-%m-%d %H:%M"))
        self.loop_var = tk.StringVar(value="1")
        self.gap_var = tk.StringVar(value="0")
        self.enabled_var = tk.BooleanVar(value=True)
        self.day_vars = {key: tk.BooleanVar(value=False) for key, _ in DAY_LABELS}

        row = 0
        ttk.Label(right, text="名称").grid(row=row, column=0, sticky="w", pady=3)
        ttk.Entry(right, textvariable=self.name_var).grid(row=row, column=1, columnspan=3,
                                                          sticky="ew", pady=3)
        row += 1

        ttk.Label(right, text="脚本目录").grid(row=row, column=0, sticky="w", pady=3)
        dir_box = ttk.Frame(right)
        dir_box.grid(row=row, column=1, columnspan=3, sticky="ew", pady=3)
        dir_box.columnconfigure(0, weight=1)
        ttk.Entry(dir_box, textvariable=self.dir_var).grid(row=0, column=0, sticky="ew")
        ttk.Button(dir_box, text="浏览", width=6, command=self._browse_dir).grid(
            row=0, column=1, padx=(4, 0))
        ttk.Button(dir_box, text="默认", width=6, command=self._reset_dir).grid(
            row=0, column=2, padx=(4, 0))
        ttk.Label(dir_box, text="留空 = scripts 默认目录；相对路径相对 scripts，也可填绝对路径",
                  foreground="#777").grid(row=1, column=0, columnspan=3, sticky="w",
                                          pady=(2, 0))
        row += 1

        ttk.Label(right, text="脚本序列").grid(row=row, column=0, sticky="nw", pady=3)
        script_box = ttk.Frame(right)
        script_box.grid(row=row, column=1, columnspan=3, sticky="ew", pady=3)
        script_box.columnconfigure(0, weight=1)
        self.pick_combo = ttk.Combobox(script_box, textvariable=self.pick_var,
                                       values=self.script_names, state="readonly")
        self.pick_combo.grid(row=0, column=0, sticky="ew")
        ttk.Button(script_box, text="添加", width=6, command=self.on_add_script).grid(
            row=0, column=1, padx=(4, 0))
        list_frame = ttk.Frame(script_box)
        list_frame.grid(row=1, column=0, sticky="ew", pady=(4, 0))
        list_frame.columnconfigure(0, weight=1)
        self.script_list = tk.Listbox(list_frame, height=5, exportselection=False,
                                      font=("Microsoft YaHei UI", 9))
        self.script_list.grid(row=0, column=0, sticky="nsew")
        list_scroll = ttk.Scrollbar(list_frame, orient="vertical",
                                    command=self.script_list.yview)
        list_scroll.grid(row=0, column=1, sticky="ns")
        self.script_list.configure(yscrollcommand=list_scroll.set)
        side = ttk.Frame(script_box)
        side.grid(row=1, column=1, sticky="n", padx=(4, 0))
        ttk.Button(side, text="上移", width=6,
                   command=lambda: self.move_script(-1)).pack(fill="x")
        ttk.Button(side, text="下移", width=6,
                   command=lambda: self.move_script(1)).pack(fill="x", pady=(4, 0))
        ttk.Button(side, text="移除", width=6, command=self.remove_script).pack(
            fill="x", pady=(4, 0))
        ttk.Label(script_box, text="按列表从上到下依次执行", foreground="#777").grid(
            row=2, column=0, sticky="w", pady=(2, 0))
        row += 1

        ttk.Label(right, text="触发").grid(row=row, column=0, sticky="w", pady=3)
        self.type_combo = ttk.Combobox(right, textvariable=self.type_var, state="readonly",
                                       values=list(TYPE_LABELS.values()))
        self.type_combo.grid(row=row, column=1, columnspan=3, sticky="ew", pady=3)
        self.type_combo.bind("<<ComboboxSelected>>", lambda _e: self._sync_fields())
        row += 1

        self.daily_box = ttk.LabelFrame(right, text="每天 / 每周", padding=(8, 4))
        self.daily_box.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(4, 0))
        ttk.Label(self.daily_box, text="时刻").pack(side="left")
        ttk.Entry(self.daily_box, textvariable=self.at_var, width=8).pack(side="left", padx=(4, 10))
        for key, label in DAY_LABELS:
            ttk.Checkbutton(self.daily_box, text=label,
                            variable=self.day_vars[key]).pack(side="left")
        ttk.Label(self.daily_box, text="（不勾=每天）", foreground="#777").pack(side="left", padx=(6, 0))
        row += 1

        self.interval_box = ttk.LabelFrame(right, text="固定间隔", padding=(8, 4))
        self.interval_box.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(4, 0))
        ttk.Label(self.interval_box, text="每").pack(side="left")
        ttk.Entry(self.interval_box, textvariable=self.minutes_var, width=8).pack(side="left", padx=4)
        ttk.Label(self.interval_box, text="分钟").pack(side="left")
        row += 1

        self.once_box = ttk.LabelFrame(right, text="一次性", padding=(8, 4))
        self.once_box.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(4, 0))
        ttk.Label(self.once_box, text="时刻").pack(side="left")
        self.once_btn = ttk.Button(self.once_box, textvariable=self.once_var, width=20,
                                   command=self._pick_once_time)
        self.once_btn.pack(side="left", padx=4)
        ttk.Label(self.once_box, text="点击选择日期与时间", foreground="#777").pack(side="left")
        row += 1

        extra = ttk.Frame(right)
        extra.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(6, 0))
        ttk.Label(extra, text="轮数").pack(side="left")
        ttk.Entry(extra, textvariable=self.loop_var, width=6).pack(side="left", padx=(4, 12))
        ttk.Label(extra, text="脚本间隔").pack(side="left")
        ttk.Entry(extra, textvariable=self.gap_var, width=7).pack(side="left", padx=(4, 2))
        ttk.Label(extra, text="ms（上一个脚本跑完后等待）", foreground="#777").pack(side="left")
        ttk.Checkbutton(extra, text="启用", variable=self.enabled_var).pack(side="left", padx=(12, 0))
        row += 1

        buttons = ttk.Frame(right)
        buttons.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(10, 0))
        ttk.Button(buttons, text="新增", width=7, command=self.on_add).pack(side="left")
        ttk.Button(buttons, text="应用修改", width=9, command=self.on_apply).pack(
            side="left", padx=(6, 0))
        ttk.Button(buttons, text="删除", width=7, command=self.on_delete).pack(
            side="left", padx=(6, 0))

        bottom = ttk.Frame(outer)
        bottom.pack(fill="x", pady=(8, 0))
        ttk.Button(bottom, text="保存并生效", width=12, command=self.on_save).pack(side="left")
        ttk.Button(bottom, text="立即执行", width=9, command=self.on_run_now).pack(
            side="left", padx=(6, 0))
        ttk.Button(bottom, text="刷新状态", width=9, command=self.refresh_status).pack(
            side="left", padx=(6, 0))
        self.hint = ttk.Label(bottom, text="", foreground="#555")
        self.hint.pack(side="right")

        self._sync_fields()

    def _sync_fields(self) -> None:
        kind = self._type_key()
        for box in (self.daily_box, self.interval_box, self.once_box):
            box.grid_remove()
        {"daily": self.daily_box, "interval": self.interval_box,
         "once": self.once_box}[kind].grid()

    def _type_key(self) -> str:
        label = self.type_var.get()
        for key, text in TYPE_LABELS.items():
            if text == label:
                return key
        return "daily"

    # -- 脚本目录 ---------------------------------------------------------
    def _current_dir(self) -> str:
        """当前任务脚本目录的绝对路径（空 = 默认 scripts 根目录）。"""
        raw = self.dir_var.get().strip()
        if not raw:
            return self.scripts_root
        if os.path.isabs(raw):
            return raw
        return os.path.join(self.scripts_root, raw)

    def _dir_scripts(self) -> list[str]:
        folder = self._current_dir()
        try:
            names = [os.path.splitext(f)[0] for f in os.listdir(folder)
                     if f.lower().endswith(".json")]
        except OSError:
            return []
        return sorted(names)

    def _refresh_script_choices(self) -> None:
        names = self._dir_scripts()
        self.pick_combo.configure(values=names)
        if not names:
            self._hint("目录里没有脚本：%s" % self._current_dir())

    def _browse_dir(self) -> None:
        from tkinter import filedialog

        current = self._current_dir()
        start = current if os.path.isdir(current) else self.scripts_root
        chosen = filedialog.askdirectory(parent=self, initialdir=start, title="选择脚本目录")
        if not chosen:
            return
        chosen = os.path.normpath(chosen)
        root = os.path.normpath(self.scripts_root)
        try:
            rel = os.path.relpath(chosen, root)
        except ValueError:
            rel = chosen
        self.dir_var.set("" if rel == "." else rel)
        self._refresh_script_choices()

    def _reset_dir(self) -> None:
        self.dir_var.set("")
        self._refresh_script_choices()

    def _refresh_scripts(self) -> None:
        self.script_list.delete(0, "end")
        for name in self.scripts:
            self.script_list.insert("end", name)

    # -- 脚本序列 ---------------------------------------------------------
    def on_add_script(self) -> None:
        name = self.pick_var.get().strip()
        if not name:
            return
        if len(self.scripts) >= MAX_SCRIPTS:
            messagebox.showinfo("上限", "一个任务最多 %d 个脚本" % MAX_SCRIPTS, parent=self)
            return
        if name in self.scripts:
            self.script_list.selection_clear(0, "end")
            self.script_list.selection_set(self.scripts.index(name))
            return
        self.scripts.append(name)
        self._refresh_scripts()
        self.script_list.selection_clear(0, "end")
        self.script_list.selection_set(len(self.scripts) - 1)

    def move_script(self, delta: int) -> None:
        sel = self.script_list.curselection()
        if not sel:
            return
        idx = sel[0]
        target = idx + delta
        if target < 0 or target >= len(self.scripts):
            return
        self.scripts[idx], self.scripts[target] = self.scripts[target], self.scripts[idx]
        self._refresh_scripts()
        self.script_list.selection_clear(0, "end")
        self.script_list.selection_set(target)

    def remove_script(self) -> None:
        sel = self.script_list.curselection()
        if not sel:
            return
        del self.scripts[sel[0]]
        self._refresh_scripts()

    # -- 列表 -------------------------------------------------------------
    def _refresh_tree(self) -> None:
        self.tree.delete(*self.tree.get_children())
        for idx, job in enumerate(self.jobs):
            self.tree.insert("", "end", iid=str(idx), values=(
                "✓" if job.get("enabled", True) else "",
                job.get("name", ""),
                describe_when(job),
                self.next_map.get(job.get("id", ""), ""),
            ))

    def _on_select(self, _event=None) -> None:
        sel = self.tree.selection()
        if not sel:
            return
        self._selected = int(sel[0])
        job = self.jobs[self._selected]
        when = job.get("when") or {}
        kind = when.get("type", "daily")
        self.name_var.set(job.get("name", ""))
        self.dir_var.set(str(job.get("dir", "") or ""))
        self._refresh_script_choices()
        self.scripts = job_scripts(job)
        self._refresh_scripts()
        self.type_var.set(TYPE_LABELS.get(kind, TYPE_LABELS["daily"]))
        self.enabled_var.set(bool(job.get("enabled", True)))
        self.loop_var.set(str(job.get("loop", 1) or 1))
        self.gap_var.set(str(job.get("gap_ms", 0) or 0))
        self.at_var.set(when.get("at", "21:30") if kind == "daily" else "21:30")
        self.minutes_var.set(str(when.get("minutes", 30) or 30))
        if kind == "once":
            self.once_var.set(when.get("at", "") or time.strftime("%Y-%m-%d %H:%M"))
        days = {str(d) for d in (when.get("days") or [])}
        for key, var in self.day_vars.items():
            var.set(key in days)
        self._sync_fields()

    # -- 动作 -------------------------------------------------------------
    def _pick_once_time(self) -> None:
        """弹日期+时间选择器，确认后写回 once_var。"""
        picked = DateTimePicker(self, self.once_var.get()).result
        if picked:
            self.once_var.set(picked)

    def _form_job(self, base: dict) -> dict | None:
        if not self.scripts:
            messagebox.showwarning("缺少脚本", "先在「脚本序列」里添加至少一个脚本", parent=self)
            return None
        kind = self._type_key()
        when: dict = {"type": kind}
        if kind == "daily":
            at = self.at_var.get().strip()
            if not _valid_hhmm(at):
                messagebox.showwarning("时间格式", "时刻要写成 HH:MM，例如 21:30", parent=self)
                return None
            when["at"] = at
            when["days"] = sorted(int(k) for k, v in self.day_vars.items() if v.get())
        elif kind == "interval":
            try:
                minutes = int(self.minutes_var.get())
            except ValueError:
                minutes = 0
            if minutes <= 0:
                messagebox.showwarning("间隔", "分钟数要是正整数", parent=self)
                return None
            when["minutes"] = minutes
        else:
            at = self.once_var.get().strip()
            if not _valid_datetime(at):
                messagebox.showwarning("时间格式", "一次性时间要写成 YYYY-MM-DD HH:MM", parent=self)
                return None
            when["at"] = at

        try:
            loop = max(1, int(self.loop_var.get()))
        except ValueError:
            loop = 1
        try:
            gap_ms = max(0, int(self.gap_var.get()))
        except ValueError:
            gap_ms = 0
        job = dict(base)
        job.update({
            "name": self.name_var.get().strip() or "任务",
            "dir": self.dir_var.get().strip(),
            "scripts": list(self.scripts),
            "enabled": bool(self.enabled_var.get()),
            "loop": loop,
            "gap_ms": gap_ms,
            "when": when,
        })
        job.pop("script", None)
        job.setdefault("id", uuid.uuid4().hex[:8])
        return job

    def on_add(self) -> None:
        if len(self.jobs) >= MAX_JOBS:
            messagebox.showinfo("上限", "最多 %d 个任务" % MAX_JOBS, parent=self)
            return
        job = self._form_job(default_job())
        if job is None:
            return
        self.jobs.append(job)
        self._selected = len(self.jobs) - 1
        self._refresh_tree()
        self._hint("已新增，记得「保存并生效」")

    def on_apply(self) -> None:
        if not (0 <= self._selected < len(self.jobs)):
            messagebox.showinfo("提示", "先在列表里选一个任务", parent=self)
            return
        job = self._form_job(self.jobs[self._selected])
        if job is None:
            return
        self.jobs[self._selected] = job
        self._refresh_tree()
        self._hint("已修改，记得「保存并生效」")

    def on_delete(self) -> None:
        if not (0 <= self._selected < len(self.jobs)):
            return
        if not messagebox.askyesno("删除", "删除选中的任务？", parent=self):
            return
        del self.jobs[self._selected]
        self._selected = -1
        self.scripts = []
        self._refresh_scripts()
        self._refresh_tree()
        self._hint("已删除，记得「保存并生效」")

    def on_run_now(self) -> None:
        """立即执行选中任务的脚本（不等触发时刻，不要求先保存到文件）。"""
        if not (0 <= self._selected < len(self.jobs)):
            messagebox.showinfo("提示", "先在列表里选一个任务", parent=self)
            return
        job = self.jobs[self._selected]
        scripts = job_scripts(job)
        if not scripts:
            messagebox.showwarning("缺少脚本", "这个任务还没有脚本", parent=self)
            return
        try:
            loop = max(1, int(job.get("loop", 1) or 1))
        except (TypeError, ValueError):
            loop = 1
        try:
            gap_ms = max(0, int(job.get("gap_ms", 0) or 0))
        except (TypeError, ValueError):
            gap_ms = 0
        reply = engine_call({"cmd": "run_scripts", "scripts": scripts, "loop": loop,
                             "dir": str(job.get("dir", "") or ""),
                             "gap_ms": gap_ms}, want="ok")
        if reply is None:
            self._hint("引擎未响应（应用没开？）")
            return
        self.log("sched: 立即执行 %s（%d 个脚本）" % (job.get("name", ""), len(scripts)))
        self._hint("已提交执行：%s" % " → ".join(scripts))

    def on_save(self) -> None:
        if not save_jobs(self.jobs):
            messagebox.showerror("保存失败", "无法写入 %s" % schedule_path(), parent=self)
            return
        self.log("sched: 已保存 %d 个任务 -> %s" % (len(self.jobs), schedule_path()))
        reply = engine_call({"cmd": "reload_schedule"}, want="schedule")
        if reply is None:
            self._hint("已保存；引擎未响应（应用没开？）")
            return
        self._apply_status(reply)
        self._hint("已生效，下次运行：%s" % (reply.get("next") or "-"))

    def refresh_status(self) -> None:
        reply = engine_call({"cmd": "get_schedule"}, want="schedule")
        if reply is None:
            self._hint("引擎未响应（应用没开，或正在执行脚本）")
            return
        self._apply_status(reply)
        self._hint("下次运行：%s" % (reply.get("next") or "-"))

    def _apply_status(self, reply: dict) -> None:
        self.next_map = {j.get("id", ""): j.get("next", "")
                         for j in reply.get("jobs", []) if isinstance(j, dict)}
        self._refresh_tree()

    def _hint(self, text: str) -> None:
        self.hint.config(text=text)


def _valid_hhmm(text: str) -> bool:
    try:
        hour, minute = (int(p) for p in text.split(":"))
    except (ValueError, TypeError):
        return False
    return 0 <= hour <= 23 and 0 <= minute <= 59


def _valid_datetime(text: str) -> bool:
    try:
        time.strptime(text, "%Y-%m-%d %H:%M")
    except ValueError:
        return False
    return True
