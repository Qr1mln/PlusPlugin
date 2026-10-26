// ============================================================================
// input_win32.cpp - PP_* 输入原语 + 键名/组合键解析
//
// 全部走 PostMessage：非阻塞，且不在目标输入线程上做 AttachThreadInput /
// SendMessageTimeout（后者会和目标 UI 线程互相死锁，导致应用卡死）。
// 消息要投给 FLUTTERVIEW 子窗口，投给顶层 runner 窗口没人处理。
// ============================================================================
#include "engine.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

UINT DownMessage(int button) {
    if (button == 1) return WM_RBUTTONDOWN;
    if (button == 2) return WM_MBUTTONDOWN;
    return WM_LBUTTONDOWN;
}

UINT UpMessage(int button) {
    if (button == 1) return WM_RBUTTONUP;
    if (button == 2) return WM_MBUTTONUP;
    return WM_LBUTTONUP;
}

WPARAM DownFlag(int button) {
    if (button == 1) return MK_RBUTTON;
    if (button == 2) return MK_MBUTTON;
    return MK_LBUTTON;
}

bool IsExtendedVk(int vk) {
    switch (vk) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_INSERT: case VK_DELETE: case VK_SNAPSHOT:
    case VK_LWIN: case VK_RWIN: case VK_NUMLOCK:
        return true;
    default:
        return false;
    }
}

DWORD KeyLParam(int vk, bool up) {
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    DWORD lp = 1u;
    lp |= (static_cast<DWORD>(scan) & 0xFFu) << 16;
    if (IsExtendedVk(vk)) lp |= 1u << 24;
    if (up) lp |= (1u << 30) | (1u << 31);
    return lp;
}

UINT DownMsg(bool sys) { return sys ? WM_SYSKEYDOWN : WM_KEYDOWN; }
UINT UpMsg(bool sys) { return sys ? WM_SYSKEYUP : WM_KEYUP; }

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

namespace se {

// 键名 -> 虚拟键码：单字符直接取值，常见键名查表，F1~F24 单独处理，
// 其余交给 VkKeyScanW（能覆盖标点、符号等）。
int NameToVk(const std::string& name) {
    const std::string n = Lower(name);
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return toupper(n[0]);
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return n[0];
    struct { const char* n; int v; } tbl[] = {
        {"enter", 0x0D}, {"return", 0x0D}, {"esc", 0x1B}, {"escape", 0x1B},
        {"tab", 0x09}, {"space", 0x20}, {"backspace", 0x08}, {"delete", 0x2E},
        {"del", 0x2E}, {"insert", 0x2D}, {"home", 0x24}, {"end", 0x23},
        {"pageup", 0x21}, {"pagedown", 0x22}, {"up", 0x26}, {"down", 0x28},
        {"left", 0x25}, {"right", 0x27}, {"pause", 0x13}, {"capslock", 0x14},
        {"numlock", 0x90}, {"scrolllock", 0x91}, {"printscreen", 0x2C}
    };
    for (const auto& t : tbl) {
        if (n == t.n) return t.v;
    }
    if (n.size() >= 2 && n[0] == 'f') {
        const int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) return 0x70 + (f - 1);
    }
    if (!n.empty()) {
        wchar_t wc[2] = {0};
        MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, wc, 2);
        const SHORT r = VkKeyScanW(wc[0]);
        if (r != 0xFFFF) return (r & 0xFF);
    }
    return 0;
}

// "Ctrl+Shift+A" -> vk='A' + 三个修饰键标志；解析不出主键返回 false。
bool ParseKeyCombo(const std::string& expr, int& vk, bool& ctrl, bool& alt,
                   bool& shift, bool& win) {
    ctrl = alt = shift = win = false;
    vk = 0;
    std::string cur;
    const auto consume = [&](const std::string& raw) {
        const std::string s = Lower(raw);
        if (s == "ctrl" || s == "control") { ctrl = true; return true; }
        if (s == "alt") { alt = true; return true; }
        if (s == "shift") { shift = true; return true; }
        if (s == "win" || s == "super") { win = true; return true; }
        const int v = NameToVk(raw);
        if (v == 0) return false;
        vk = v;
        return true;
    };
    for (char c : expr) {
        if (c == '+') {
            if (!consume(cur)) return false;
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!consume(cur)) return false;
    return vk != 0;
}

}  // namespace se

// ---------------------------------------------------------------------------
// 导出原语
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) int __cdecl PP_Mouse(HWND view, int x, int y,
                                                      int button, int action) {
    if (view == nullptr || !IsWindow(view)) return PP_ERR_PARAM;
    if (button < 0 || button > 2) return PP_ERR_PARAM;
    if (action < 0 || action > 3) return PP_ERR_PARAM;
    const LPARAM point = MAKELPARAM(x, y);
    const WPARAM flag = DownFlag(button);
    switch (action) {
    case 0:
        PostMessageW(view, WM_MOUSEMOVE, 0, point);
        break;
    case 1:
        PostMessageW(view, WM_MOUSEMOVE, flag, point);
        PostMessageW(view, DownMessage(button), flag, point);
        break;
    case 2:
        PostMessageW(view, UpMessage(button), 0, point);
        break;
    case 3:
        PostMessageW(view, WM_MOUSEMOVE, flag, point);
        PostMessageW(view, DownMessage(button), flag, point);
        PostMessageW(view, UpMessage(button), 0, point);
        break;
    default:
        return PP_ERR_PARAM;
    }
    return PP_OK;
}

extern "C" __declspec(dllexport) int __cdecl PP_Delay(int ms) {
    if (ms < 0) return PP_ERR_PARAM;
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
    return PP_OK;
}

extern "C" __declspec(dllexport) int __cdecl PP_FindImage(HWND view, const char* image,
                                                          double threshold,
                                                          int timeout_ms) {
    (void)view;
    (void)image;
    (void)threshold;
    (void)timeout_ms;
    return PP_ERR_NOT_IMPLEMENTED;
}

extern "C" __declspec(dllexport) int __cdecl PP_InputText(HWND view, const char* text) {
    if (view == nullptr || !IsWindow(view) || text == nullptr) return PP_ERR_PARAM;
    const int len = static_cast<int>(strlen(text));
    if (len <= 0) return 0;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, text, len, nullptr, 0);
    if (wlen <= 0) return 0;
    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, len, &wbuf[0], wlen);
    int sent = 0;
    for (wchar_t w : wbuf) {
        const wchar_t c = (w == L'\n' || w == L'\r') ? L'\r' : w;
        if (PostMessageW(view, WM_CHAR, static_cast<WPARAM>(c), 0)) ++sent;
    }
    return sent;
}

extern "C" __declspec(dllexport) int __cdecl PP_Key(HWND view, int vk, int ctrl, int alt,
                                                    int shift, int win, int action,
                                                    int real) {
    if (view == nullptr || !IsWindow(view)) return PP_ERR_PARAM;
    if (vk <= 0 || vk > 0xFFFF) return PP_ERR_PARAM;
    if (action < 0 || action > 2) return PP_ERR_PARAM;
    if (real != 0) return PP_ERR_STATE;

    int mods[4] = {0, 0, 0, 0};
    int nmods = 0;
    if (ctrl) mods[nmods++] = VK_CONTROL;
    if (alt) mods[nmods++] = VK_MENU;
    if (shift) mods[nmods++] = VK_SHIFT;
    if (win) mods[nmods++] = VK_LWIN;
    const bool sys = (alt != 0);

    // 修饰键的 WM_KEYDOWN 会排在主体键之前进入目标消息队列，由目标自身按顺序
    // 处理，其键盘状态也随之更新，足以覆盖绝大多数应用。
    struct Step { int vk; bool up; };
    Step steps[8];
    int nsteps = 0;
    const bool want_down = (action == 0 || action == 1);
    const bool want_up = (action == 0 || action == 2);
    if (want_down) {
        for (int i = 0; i < nmods; ++i) steps[nsteps++] = {mods[i], false};
        steps[nsteps++] = {vk, false};
    }
    if (want_up) {
        steps[nsteps++] = {vk, true};
        for (int i = nmods - 1; i >= 0; --i) steps[nsteps++] = {mods[i], true};
    }

    int sent = 0;
    for (int i = 0; i < nsteps; ++i) {
        const LPARAM lp = static_cast<LPARAM>(KeyLParam(steps[i].vk, steps[i].up));
        if (PostMessageW(view, steps[i].up ? UpMsg(sys) : DownMsg(sys), steps[i].vk, lp)) {
            ++sent;
        }
    }
    return sent;
}

extern "C" __declspec(dllexport) int __cdecl PP_Version(void) {
    return 4;
}
