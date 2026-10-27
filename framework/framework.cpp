// ============================================================================
// framework.cpp - MVP 的 framework.dll 实现。
//
// 职责（MVP 只验证一条闭环）：
//   浏览器 --WebSocket--> framework.dll --> 宿主窗口（动鼠标/按键/等待）--> 回日志
//
// 设计要点：
//   - 作为 pplugin 插件，经 GetDesc/init(const Host*) 拿到 proxy 交来的能力
//     （Host: publishLog / mainWindow / viewWindow / scale）。
//   - 用 CivetWeb（libs/civetweb，C，裸 TCP/WS，零协议风险）起一个
//     127.0.0.1 的 WebSocket 服务端，URI 为 /ws；顺带把 DLL 所在目录当 document_root，
//     可直接 http 托管 index.html。
//   - 事件执行：鼠标/键盘/滚轮/文本走 PostMessage 后台投递到目标窗口（不动物理鼠标）。
//   - 每条命令执行后，把日志写回 proxy.log（host->log）并通过 WS 回一个 JSON 包。
//
// 明确不做（见 MVP 规划）：截图取点、OCR、任务调度、热重载、Web 编辑器、JS 脚本层。
// 保持纯 ASCII 源码（CivetWeb / mingw 均友好）。
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "civetweb.h"
#include "pplugin.h"
#include <nlohmann/json.hpp>

namespace {

const Host* g_host = nullptr;
struct mg_context* g_ctx = nullptr;
HMODULE g_hModule = nullptr;
std::wstring g_moduleDir;

// 把一条日志同时：1) 写进 proxy.log（host->log）；2) 收集进 logs 供 WS 回包。
void EmitLog(std::vector<std::string>& logs, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;

    std::string line(buf);
    logs.push_back(line);
    if (g_host != nullptr && g_host->publishLog != nullptr) {
        g_host->publishLog(line.c_str());
    }
}

// ---- 事件执行：后台 PostMessage 投递到目标窗口（不移动真实鼠标、不抢焦点）----
// 注意：PostMessage 不进系统输入队列，个别程序（Raw Input / 特殊自绘）可能不响应。

// ---- 坐标换算：view（FLUTTERVIEW）相对坐标 <-> 屏幕绝对坐标 ----

// 目标窗口：优先 viewWindow（真正接收输入的 Flutter 子窗口），回退 mainWindow。
HWND TargetWindow() {
    HWND h = nullptr;
    if (g_host != nullptr && g_host->viewWindow != nullptr) {
        h = g_host->viewWindow();
    }
    if (h == nullptr && g_host != nullptr && g_host->mainWindow != nullptr) {
        h = g_host->mainWindow();
    }
    return h;
}

// 目标窗口客户区的屏幕矩形（物理像素）；不可用时返回 false。
bool ViewRect(RECT& out) {
    HWND wnd = TargetWindow();
    if (wnd == nullptr) {
        return false;
    }
    RECT rc;  // 客户区大小（客户坐标）
    if (!GetClientRect(wnd, &rc)) {
        return false;
    }
    POINT tl = {0, 0};  // 客户区左上角 -> 屏幕坐标
    if (!ClientToScreen(wnd, &tl)) {
        return false;
    }
    out.left = tl.x;
    out.top = tl.y;
    out.right = tl.x + (rc.right - rc.left);
    out.bottom = tl.y + (rc.bottom - rc.top);
    if (out.right <= out.left || out.bottom <= out.top) {
        return false;
    }
    return true;
}

// 虚拟光标（客户区坐标），插件内部维护，供相对移动/拖拽使用。
int g_cx = 0;
int g_cy = 0;

// ---- Cursor readout (read-only): where the real mouse is inside the view ----
// Feeds the editor "pick coordinate" panel. It only queries state: no hooks,
// no injected messages, no hooks that swallow clicks.

// Round to 1 decimal without depending on printf formatting.
double Round1(double v) {
    return (v >= 0.0) ? std::floor(v * 10.0 + 0.5) / 10.0
                      : std::ceil(v * 10.0 - 0.5) / 10.0;
}

// Fills: sx/sy (screen), x/y (view client, physical px), lx/ly (logical px),
// w/h (view client size), scale (DPI/96), inside, f8 (F8 held down).
bool BuildCursorInfo(nlohmann::json& out) {
    HWND wnd = TargetWindow();
    if (wnd == nullptr) {
        return false;
    }
    POINT sp = {0, 0};
    if (!GetCursorPos(&sp)) {
        return false;
    }
    RECT rc;  // client area size (client coords)
    if (!GetClientRect(wnd, &rc)) {
        return false;
    }
    POINT cp = sp;
    if (!ScreenToClient(wnd, &cp)) {
        return false;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    double scale = (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0;
    if (!(scale > 0.0)) {
        scale = 1.0;
    }
    out["sx"] = sp.x;
    out["sy"] = sp.y;
    out["x"] = cp.x;
    out["y"] = cp.y;
    out["lx"] = Round1(cp.x / scale);
    out["ly"] = Round1(cp.y / scale);
    out["w"] = w;
    out["h"] = h;
    out["scale"] = scale;
    out["inside"] = (cp.x >= 0 && cp.y >= 0 && cp.x < w && cp.y < h);
    // The page has no focus while the mouse sits over the target window, so the
    // key state has to be sampled here in the host process.
    out["f8"] = ((GetAsyncKeyState(VK_F8) & 0x8000) != 0);
    return true;
}

// 向目标窗口投递鼠标消息（客户区坐标）。
void PostMouse(UINT msg, WPARAM wParam, int cx, int cy) {
    HWND w = TargetWindow();
    if (w == nullptr) {
        return;
    }
    PostMessageW(w, msg, wParam,
                 MAKELPARAM(static_cast<short>(cx), static_cast<short>(cy)));
}

// 把 (x,y)（space 指定）解析成客户区坐标；窗口不可用返回 false。
bool ResolveClient(int x, int y, const std::string& space, int& cx, int& cy) {
    HWND w = TargetWindow();
    if (w == nullptr) {
        return false;
    }
    if (space == "screen") {
        POINT p = {x, y};
        if (!ScreenToClient(w, &p)) {
            return false;
        }
        cx = p.x;
        cy = p.y;
    } else {
        cx = x;
        cy = y;
    }
    return true;
}

void ExecMove(int dx, int dy) {
    g_cx += dx;
    g_cy += dy;
    PostMouse(WM_MOUSEMOVE, 0, g_cx, g_cy);
}

void ExecMoveTo(int x, int y, const std::string& space) {
    int cx = x;
    int cy = y;
    if (!ResolveClient(x, y, space, cx, cy)) {
        return;
    }
    g_cx = cx;
    g_cy = cy;
    PostMouse(WM_MOUSEMOVE, 0, g_cx, g_cy);
}

UINT ButtonDownMsg(const std::string& button) {
    if (button == "right") {
        return WM_RBUTTONDOWN;
    }
    if (button == "middle") {
        return WM_MBUTTONDOWN;
    }
    return WM_LBUTTONDOWN;
}
UINT ButtonUpMsg(const std::string& button) {
    if (button == "right") {
        return WM_RBUTTONUP;
    }
    if (button == "middle") {
        return WM_MBUTTONUP;
    }
    return WM_LBUTTONUP;
}
WPARAM ButtonDownKey(const std::string& button) {
    if (button == "right") {
        return MK_RBUTTON;
    }
    if (button == "middle") {
        return MK_MBUTTON;
    }
    return MK_LBUTTON;
}

void ExecClick(int x, int y, const std::string& space, const std::string& button) {
    if (x >= 0 && y >= 0) {
        int cx = x;
        int cy = y;
        if (ResolveClient(x, y, space, cx, cy)) {
            g_cx = cx;
            g_cy = cy;
        }
    }
    PostMouse(WM_MOUSEMOVE, 0, g_cx, g_cy);
    PostMouse(ButtonDownMsg(button), ButtonDownKey(button), g_cx, g_cy);
    PostMouse(ButtonUpMsg(button), 0, g_cx, g_cy);
}

void ExecButton(int x, int y, const std::string& space, const std::string& button,
                bool down) {
    if (x >= 0 && y >= 0) {
        int cx = x;
        int cy = y;
        if (ResolveClient(x, y, space, cx, cy)) {
            g_cx = cx;
            g_cy = cy;
        }
    }
    PostMouse(down ? ButtonDownMsg(button) : ButtonUpMsg(button),
              down ? ButtonDownKey(button) : 0, g_cx, g_cy);
}

void ExecKey(WORD vk) {
    HWND w = TargetWindow();
    if (w == nullptr) {
        return;
    }
    PostMessageW(w, WM_KEYDOWN, vk, 0);
    PostMessageW(w, WM_KEYUP, vk, 0);
}

void ExecDelay(int ms) {
    if (ms > 0 && ms <= 60000) {
        Sleep(static_cast<DWORD>(ms));
    }
}

// 滚轮：delta 以 WHEEL_DELTA(120) 的倍数，正=上滚，负=下滚。
void ExecScroll(int delta) {
    if (delta == 0) {
        return;
    }
    HWND w = TargetWindow();
    if (w == nullptr) {
        return;
    }
    POINT p = {g_cx, g_cy};
    ClientToScreen(w, &p);
    PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(delta)),
                 MAKELPARAM(p.x, p.y));
}

// 文本输入：UTF-8 -> UTF-16，逐字符向目标窗口投递 WM_CHAR。
void ExecText(const std::string& utf8) {
    if (utf8.empty()) {
        return;
    }
    HWND w = TargetWindow();
    if (w == nullptr) {
        return;
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return;
    }
    std::wstring ws(static_cast<size_t>(wlen - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &ws[0], wlen);
    for (const wchar_t ch : ws) {
        PostMessageW(w, WM_CHAR, static_cast<WPARAM>(ch), 0);
    }
}

// ---- WebSocket 回调 ----

int WSConnect(const struct mg_connection*, void*) {
    return 0;  // 0 = 继续握手
}

void WSReady(struct mg_connection* conn, void*) {
    (void)conn;
    if (g_host != nullptr && g_host->publishLog != nullptr) {
        g_host->publishLog("websocket client connected");
    }
}

void WSClose(const struct mg_connection*, void*) {
    if (g_host != nullptr && g_host->publishLog != nullptr) {
        g_host->publishLog("websocket client disconnected");
    }
}

// 统一回包封套：{ id, success, data, code }。
// code "0000"=成功，"9999"=失败；data 成功时为 ""（geometry 为对象），失败时为错误信息。
std::string MakeReply(const std::string& id, bool success,
                      const nlohmann::json& data, const std::string& code) {
    nlohmann::json r;
    r["id"] = id;
    r["success"] = success;
    r["data"] = data;
    r["code"] = code;
    return r.dump();
}

// 解析并分发一条命令。请求：{id,type,action,space,button,...}（兼容旧 action/op）。
std::string HandleCommand(const std::string& msg, std::vector<std::string>& logs) {
    const std::string kOk = "0000";
    const std::string kErr = "9999";

    nlohmann::json cmd;
    try {
        cmd = nlohmann::json::parse(msg);
    } catch (...) {
        return MakeReply("", false, "bad json", kErr);
    }
    if (!cmd.is_object()) {
        return MakeReply("", false, "not an object", kErr);
    }

    const std::string id = cmd.value("id", std::string());
    const std::string type = cmd.value("type", std::string());
    std::string category = type;
    std::string sub;
    if (!type.empty()) {
        category = type;
        sub = cmd.value("action", std::string());
    } else {
        category = cmd.value("action", std::string());
        sub = cmd.value("op", std::string());
    }

    const std::string space = cmd.value("space", std::string("view"));
    const std::string button = cmd.value("button", std::string("left"));

    if (category == "mouse") {
        if (sub.empty()) {
            sub = "move";
        }
        if (sub == "move") {
            const int dx = cmd.value("dx", 0);
            const int dy = cmd.value("dy", 0);
            ExecMove(dx, dy);
            EmitLog(logs, "mouse move dx=%d dy=%d", dx, dy);
        } else if (sub == "moveTo") {
            const int x = cmd.value("x", -1);
            const int y = cmd.value("y", -1);
            if (x < 0 || y < 0) {
                return MakeReply(id, false, "moveTo needs x,y", kErr);
            }
            ExecMoveTo(x, y, space);
            EmitLog(logs, "mouse moveTo %s=%d,%d", space.c_str(), x, y);
        } else if (sub == "click") {
            const int x = cmd.value("x", -1);
            const int y = cmd.value("y", -1);
            ExecClick(x, y, space, button);
            EmitLog(logs, "mouse click %s=%d,%d button=%s", space.c_str(), x, y,
                    button.c_str());
        } else if (sub == "down" || sub == "up") {
            const int x = cmd.value("x", -1);
            const int y = cmd.value("y", -1);
            ExecButton(x, y, space, button, sub == "down");
            EmitLog(logs, "mouse %s %s=%d,%d button=%s", sub.c_str(), space.c_str(),
                    x, y, button.c_str());
        } else if (sub == "scroll") {
            const int delta = cmd.value("delta", 0);
            ExecScroll(delta);
            EmitLog(logs, "mouse scroll delta=%d", delta);
        } else {
            return MakeReply(id, false, "unknown mouse action", kErr);
        }
        return MakeReply(id, true, "", kOk);
    }

    if (category == "key") {
        const int vk = cmd.value("vk", 0);
        if (vk <= 0 || vk > 255) {
            return MakeReply(id, false, "invalid vk", kErr);
        }
        ExecKey(static_cast<WORD>(vk));
        EmitLog(logs, "key vk=%d", vk);
        return MakeReply(id, true, "", kOk);
    }

    if (category == "text") {
        const std::string text = cmd.value("text", std::string());
        if (text.empty()) {
            return MakeReply(id, false, "text is empty", kErr);
        }
        ExecText(text);
        EmitLog(logs, "text bytes=%d", static_cast<int>(text.size()));
        return MakeReply(id, true, "", kOk);
    }

    if (category == "delay") {
        const int ms = cmd.value("ms", 0);
        ExecDelay(ms);
        EmitLog(logs, "delay ms=%d", ms);
        return MakeReply(id, true, "", kOk);
    }

    if (category == "geometry") {
        RECT rc;
        if (!ViewRect(rc)) {
            return MakeReply(id, false, "view window unavailable", kErr);
        }
        nlohmann::json g;
        g["x"] = rc.left;
        g["y"] = rc.top;
        g["w"] = rc.right - rc.left;
        g["h"] = rc.bottom - rc.top;
        g["scale"] = (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0f;
        return MakeReply(id, true, g, kOk);
    }

    if (category == "cursor") {
        // 只读：回报真实光标在 view 内的位置，供编辑器取点面板轮询。
        nlohmann::json c;
        if (!BuildCursorInfo(c)) {
            return MakeReply(id, false, "view window unavailable", kErr);
        }
        return MakeReply(id, true, c, kOk);
    }

    return MakeReply(id, false, "unknown type", kErr);
}

int WSData(struct mg_connection* conn, int /*bits*/, char* data, size_t data_len,
           void* /*cbdata*/) {
    if (conn == nullptr || data == nullptr) {
        return 1;
    }
    // 收一条命令（MVP 消息很小，单帧足够；多帧场景留待后续）。
    std::string msg(data, data_len);
    std::vector<std::string> logs;
    // 取点轮询约 14 次/秒，回显到 proxy.log 会把日志刷爆，直接跳过。
    if (msg.find("\"cursor\"") == std::string::npos) {
        EmitLog(logs, "recv: %s", msg.c_str());
    }

    const std::string reply = HandleCommand(msg, logs);
    // 执行日志已由 EmitLog 写入 proxy.log；回包只发统一封套。
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, reply.c_str(), reply.size());
    return 1;  // 1 = 保持连接
}

// CivetWeb 的日志回调：把它的启动/运行信息（含 mg_start 失败原因）导进 proxy.log。
// 返回 1 表示抑制 civetweb 自身的 stderr 输出（我们已转发）。
int WSLog(const struct mg_connection* conn, const char* message) {
    (void)conn;
    if (g_host != nullptr && g_host->publishLog != nullptr && message != nullptr) {
        std::string s = "civetweb: ";
        s += message;
        g_host->publishLog(s.c_str());
    }
    return 1;
}

std::string ModuleDirUtf8() {
    if (g_moduleDir.empty()) {
        return ".";
    }
    char buf[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_moduleDir.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return std::string(buf);
}

// 在 mg_start 之前自己先绑一次端口，把 WSA 错误号打进 proxy.log。
// 这样即便 CivetWeb 没把 bind 失败原因通过 log_message 回调吐出来，也能定位
// （例如 WSA=10048 表示端口已被占用）。
static void ProbePort() {
    if (g_host == nullptr || g_host->publishLog == nullptr) {
        return;
    }
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        g_host->publishLog("ProbePort: WSAStartup failed");
        return;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        g_host->publishLog("ProbePort: socket() failed");
        WSACleanup();
        return;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(47801);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    int r = bind(s, (sockaddr*)&a, sizeof(a));
    if (r != 0) {
        int e = WSAGetLastError();
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "ProbePort: bind 127.0.0.1:47801 failed, WSA=%d", e);
        g_host->publishLog(buf);
    } else {
        g_host->publishLog("ProbePort: bind 127.0.0.1:47801 ok (端口可用)");
    }
    closesocket(s);
    WSACleanup();
}

// %APPDATA%\TTVoice\scripts
static std::filesystem::path ScriptsDir() {
    wchar_t appdata[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
    if (n == 0 || n >= 32768) {
        return std::filesystem::path();
    }
    return std::filesystem::path(appdata) / L"TTVoice" / L"scripts";
}

static std::string ReadTextFileUtf8(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 脚本名只允许普通文件名，禁止路径穿越。
static bool SafeScriptName(const std::string& name) {
    if (name.empty() || name.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            return false;
        }
    }
    return true;
}

// 脚本文件路径：%APPDATA%\TTVoice\scripts\<name>.json（UTF-8 名转宽字符）。
static std::filesystem::path ScriptPath(const std::string& utf8Name) {
    std::wstring wide;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), -1, nullptr, 0);
    if (n > 0) {
        wide.resize(static_cast<size_t>(n - 1));
        MultiByteToWideChar(CP_UTF8, 0, utf8Name.c_str(), -1, &wide[0], n);
    }
    return ScriptsDir() / (wide + L".json");
}

// GET    /api/scripts              列出
// GET    /api/scripts?name=xx      读取
// POST   /api/scripts?name=xx      新建/保存（body 为脚本 JSON）
// DELETE /api/scripts?name=xx      删除
static int ApiScripts(struct mg_connection* conn, void* /*cbdata*/) {
    nlohmann::json resp;
    const struct mg_request_info* ri = mg_get_request_info(conn);
    const std::string method =
        (ri != nullptr && ri->request_method != nullptr) ? ri->request_method : "GET";

    char nameBuf[256] = {};
    const bool hasName =
        (ri != nullptr && ri->query_string != nullptr &&
         mg_get_var(ri->query_string, strlen(ri->query_string), "name", nameBuf,
                    sizeof(nameBuf)) > 0);
    const std::string name(nameBuf);

    if (hasName && !SafeScriptName(name)) {
        resp["success"] = false;
        resp["data"] = "bad name";
        resp["code"] = "9999";
    } else if (hasName && method == "DELETE") {
        std::error_code ec;
        const bool removed = std::filesystem::remove(ScriptPath(name), ec);
        resp["success"] = removed;
        resp["data"] = removed ? "" : "script not found";
        resp["code"] = removed ? "0000" : "9999";
    } else if (hasName && (method == "POST" || method == "PUT")) {
        std::string payload;
        if (ri != nullptr && ri->content_length > 0 &&
            ri->content_length < 4 * 1024 * 1024) {
            payload.resize(static_cast<size_t>(ri->content_length));
            size_t got = 0;
            while (got < payload.size()) {
                const int n = mg_read(conn, &payload[got], payload.size() - got);
                if (n <= 0) {
                    break;
                }
                got += static_cast<size_t>(n);
            }
            payload.resize(got);
        }

        bool ok = false;
        std::string error = "bad json";
        try {
            const nlohmann::json j = nlohmann::json::parse(payload);
            if (j.is_object()) {
                const std::filesystem::path path = ScriptPath(name);
                std::filesystem::path tmp = path;
                tmp += L".tmp";
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (out) {
                    out << j.dump(2);
                    out.close();
                    std::error_code ec;
                    std::filesystem::rename(tmp, path, ec);
                    ok = !ec;
                    if (!ok) {
                        error = "write failed";
                        std::filesystem::remove(tmp, ec);
                    }
                } else {
                    error = "open failed";
                }
            } else {
                error = "not an object";
            }
        } catch (...) {
            error = "bad json";
        }
        resp["success"] = ok;
        resp["data"] = ok ? "" : error;
        resp["code"] = ok ? "0000" : "9999";
    } else if (hasName) {
        const std::string text = ReadTextFileUtf8(ScriptPath(name));
        if (text.empty()) {
            resp["success"] = false;
            resp["data"] = "script not found";
            resp["code"] = "9999";
        } else {
            try {
                resp["data"] = nlohmann::json::parse(text);
                resp["success"] = true;
                resp["code"] = "0000";
            } catch (...) {
                resp["success"] = false;
                resp["data"] = "bad json";
                resp["code"] = "9999";
            }
        }
    } else {
        nlohmann::json arr = nlohmann::json::array();
        std::error_code ec;
        const std::filesystem::path dir = ScriptsDir();
        std::vector<std::string> names;
        if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (_wcsicmp(entry.path().extension().c_str(), L".json") != 0) {
                    continue;
                }
                names.push_back(entry.path().stem().u8string());
            }
        }
        std::sort(names.begin(), names.end());
        for (const std::string& s : names) {
            arr.push_back(s);
        }
        resp["success"] = true;
        resp["data"] = arr;
        resp["code"] = "0000";
    }

    const std::string body = resp.dump();
    mg_send_http_ok(conn, "application/json; charset=utf-8",
                    static_cast<long long>(body.size()));
    mg_write(conn, body.data(), body.size());
    return 200;
}

// GET /api/app: 目标窗口客户区几何 + DPI 缩放（供“刷新应用”展示）。
static int ApiApp(struct mg_connection* conn, void* /*cbdata*/) {
    nlohmann::json resp;
    RECT rc;
    if (!ViewRect(rc)) {
        resp["success"] = false;
        resp["data"] = "view window unavailable";
        resp["code"] = "9999";
    } else {
        nlohmann::json g;
        g["x"] = rc.left;
        g["y"] = rc.top;
        g["w"] = rc.right - rc.left;
        g["h"] = rc.bottom - rc.top;
        g["scale"] = (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0f;
        g["app"] = "TTVoice.exe";
        resp["success"] = true;
        resp["data"] = g;
        resp["code"] = "0000";
    }

    const std::string body = resp.dump();
    mg_send_http_ok(conn, "application/json; charset=utf-8",
                    static_cast<long long>(body.size()));
    mg_write(conn, body.data(), body.size());
    return 200;
}

// GET /api/cursor: live cursor readout in view client coords (read-only).
// Same payload as the WS {"type":"cursor"} branch, so both callers agree.
static int ApiCursor(struct mg_connection* conn, void* /*cbdata*/) {
    nlohmann::json resp;
    nlohmann::json cur;
    if (!BuildCursorInfo(cur)) {
        resp["success"] = false;
        resp["data"] = "view window unavailable";
        resp["code"] = "9999";
    } else {
        resp["success"] = true;
        resp["data"] = cur;
        resp["code"] = "0000";
    }

    const std::string body = resp.dump();
    mg_send_http_ok(conn, "application/json; charset=utf-8",
                    static_cast<long long>(body.size()));
    mg_write(conn, body.data(), body.size());
    return 200;
}

}  // namespace

// pplugin 插件入口：Desc::init(const Host*) 由 framework 的 plugin_loader 调用。
static int __cdecl PluginInit(const Host* host) {
    if (host == nullptr || host->abi != P_ABI || host->structSize < sizeof(Host)) {
        return ERR_STATE;
    }
    if (host->publishLog == nullptr) {
        return ERR_PARAM;
    }
    g_host = host;

    const std::string docRoot = ModuleDirUtf8();
    const char* options[] = {
        "listening_ports", "127.0.0.1:47801",
        "document_root",   docRoot.c_str(),
        "enable_directory_listing", "no",
        "request_timeout_ms", "30000",  // CivetWeb 只认 *_ms，且无 enable_websocket 选项
        nullptr,
    };

    struct mg_callbacks cb = {};
    cb.log_message = &WSLog;  // 把 CivetWeb 启动/运行日志导进 proxy.log
    ProbePort();  // 先自测端口是否可绑定（结果见 proxy.log 的 [framework] ProbePort）
    g_ctx = mg_start(&cb, nullptr, options);
    if (g_ctx == nullptr) {
        host->publishLog("mg_start failed");
        return ERR_STATE;
    }
    mg_set_websocket_handler(g_ctx, "/ws", &WSConnect, &WSReady, &WSData, &WSClose,
                             nullptr);
    mg_set_request_handler(g_ctx, "/api/scripts", &ApiScripts, nullptr);
    mg_set_request_handler(g_ctx, "/api/app", &ApiApp, nullptr);
    mg_set_request_handler(g_ctx, "/api/cursor", &ApiCursor, nullptr);

    std::string ok = "started, ws=ws://127.0.0.1:47801/ws doc_root=";
    ok += docRoot;
    host->publishLog(ok.c_str());
    return P_OK;
}

static int __cdecl PluginTick(void) {
    return P_OK;
}

static int __cdecl PluginShutdown(void) {
    if (g_ctx != nullptr) {
        mg_stop(g_ctx);
        g_ctx = nullptr;
    }
    return P_OK;
}

static const Desc g_desc = {
    P_ABI,
    static_cast<uint32_t>(sizeof(Desc)),
    "framework",
    "1.0",
    &PluginInit,
    &PluginTick,
    &PluginShutdown,
};

extern "C" __declspec(dllexport) const Desc* __cdecl GetDesc(void) {
    return &g_desc;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        wchar_t buf[MAX_PATH] = {};
        if (GetModuleFileNameW(hModule, buf, MAX_PATH) > 0) {
            g_moduleDir = std::wstring(buf);
            // 去掉文件名，保留目录（含末尾分隔符由 parent_path 处理）。
            g_moduleDir = std::wstring(
                std::filesystem::path(g_moduleDir).parent_path().wstring());
        }
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_ctx != nullptr) {
            mg_stop(g_ctx);
            g_ctx = nullptr;
        }
    }
    return TRUE;
}
