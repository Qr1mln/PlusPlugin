r"""脚本事件模型：类型/标签常量、键名解析、事件清洗与展示。

事件用普通 dict 表示（与 JSON 格式一一对应）；本模块只提供常量与纯函数，
不碰 UI，也不发网络请求。
"""
from __future__ import annotations

from .win32 import user32

# --- 事件类型 -------------------------------------------------------------
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


# --- 坐标 / 文本格式化 ----------------------------------------------------
def fmt_coord(v) -> str:
    v = round(float(v), 1)
    return str(int(v)) if v.is_integer() else str(v)


def text_preview(text: str, limit: int = 24) -> str:
    """把多行文本压成一行摘要，给表格/日志用。"""
    flat = " ".join((text or "").split())
    if not flat:
        return "(空)"
    return flat if len(flat) <= limit else flat[:limit] + "…"


# --- 键名解析 -------------------------------------------------------------
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


# --- 事件清洗 / 展示 -------------------------------------------------------
def default_event(type_key: str) -> dict:
    """新事件的默认字段。"""
    if type_key == TYPE_DELAY:
        return {"type": TYPE_DELAY, "ms": 500, "remark": ""}
    if type_key == TYPE_FIND:
        return {"type": TYPE_FIND, "image": "", "threshold": 0.9,
                "timeout_ms": 3000, "x": 0, "y": 0, "remark": ""}
    if type_key == TYPE_INPUT:
        return {"type": TYPE_INPUT, "text": "", "remark": ""}
    if type_key == TYPE_KEY:
        return {"type": TYPE_KEY, "key": "Enter", "action": "press", "remark": ""}
    return {"type": TYPE_MOUSE, "action": "click", "button": "left", "x": 0, "y": 0,
            "remark": ""}


def clean_event(ev: dict) -> dict:
    """把任意来源的事件规整成标准字段（兼容旧格式）。"""
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


def event_detail(ev: dict) -> str:
    """事件表「参数」列的展示文本。"""
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