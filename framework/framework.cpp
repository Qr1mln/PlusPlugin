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
//   - 事件执行只做 3 个：鼠标移动/点击（SendInput）、键盘按键（SendInput）、延迟（Sleep）。
//   - 每条命令执行后，把日志写回 proxy.log（host->log）并通过 WS 回一个 JSON 包。
//
// 明确不做（见 MVP 规划）：截图取点、OCR、任务调度、热重载、Web 编辑器、JS 脚本层。
// 保持纯 ASCII 源码（CivetWeb / mingw 均友好）。
// ============================================================================

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
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

// ---- 三个事件执行（直接用 SendInput，独立于宿主窗口句柄也能产生可见动作）----

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

// view 相对坐标 -> 屏幕绝对坐标。窗口不可用时返回 false（调用方按屏幕坐标兜底）。
bool ViewToScreen(int x, int y, int& sx, int& sy) {
    RECT rc;
    if (!ViewRect(rc)) {
        return false;
    }
    sx = rc.left + x;
    sy = rc.top + y;
    return true;
}

// 以绝对方式把光标移到屏幕点（按虚拟桌面归一化，支持多显示器）。
void MoveToScreen(int sx, int sy) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) {
        return;
    }
    INPUT m = {};
    m.type = INPUT_MOUSE;
    m.mi.dx = static_cast<LONG>((static_cast<long long>(sx - vx) * 65535) / (vw - 1));
    m.mi.dy = static_cast<LONG>((static_cast<long long>(sy - vy) * 65535) / (vh - 1));
    m.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &m, sizeof(m));
}

// 相对移动（保留旧语义）。
void ExecMove(int dx, int dy) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// button -> SendInput 的按下/抬起标志。
DWORD ButtonDownFlag(const std::string& button) {
    if (button == "right") {
        return MOUSEEVENTF_RIGHTDOWN;
    }
    if (button == "middle") {
        return MOUSEEVENTF_MIDDLEDOWN;
    }
    return MOUSEEVENTF_LEFTDOWN;
}
DWORD ButtonUpFlag(const std::string& button) {
    if (button == "right") {
        return MOUSEEVENTF_RIGHTUP;
    }
    if (button == "middle") {
        return MOUSEEVENTF_MIDDLEUP;
    }
    return MOUSEEVENTF_LEFTUP;
}

// view 相对 / screen 绝对 -> 屏幕点。space != "screen" 时按 view 换算。
void ResolvePoint(int x, int y, const std::string& space, int& sx, int& sy) {
    sx = x;
    sy = y;
    if (space != "screen") {
        ViewToScreen(x, y, sx, sy);  // 窗口不可用时按屏幕坐标兜底
    }
}

// 绝对移动。space: "view"(默认) / "screen"。
void ExecMoveTo(int x, int y, const std::string& space) {
    int sx = x;
    int sy = y;
    ResolvePoint(x, y, space, sx, sy);
    MoveToScreen(sx, sy);
}

// 点击：x,y<0 表示在当前光标处点击。button: left/right/middle。
void ExecClick(int x, int y, const std::string& space, const std::string& button) {
    if (x >= 0 && y >= 0) {
        int sx = x;
        int sy = y;
        ResolvePoint(x, y, space, sx, sy);
        MoveToScreen(sx, sy);
    }
    INPUT down = {};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = ButtonDownFlag(button);
    SendInput(1, &down, sizeof(down));
    INPUT up = {};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = ButtonUpFlag(button);
    SendInput(1, &up, sizeof(up));
}

// 按下/抬起（拖拽用）。
void ExecButton(int x, int y, const std::string& space, const std::string& button,
                bool down) {
    if (x >= 0 && y >= 0) {
        int sx = x;
        int sy = y;
        ResolvePoint(x, y, space, sx, sy);
        MoveToScreen(sx, sy);
    }
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? ButtonDownFlag(button) : ButtonUpFlag(button);
    SendInput(1, &in, sizeof(in));
}

void ExecKey(WORD vk) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;
    SendInput(1, &down, sizeof(down));
    INPUT up = {};
    up.type = INPUT_KEYBOARD;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(up));
}

void ExecDelay(int ms) {
    if (ms > 0 && ms <= 60000) {
        Sleep(static_cast<DWORD>(ms));
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
    EmitLog(logs, "recv: %s", msg.c_str());

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
