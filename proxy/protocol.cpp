// protocol.cpp - implementation of the ghost side of the version protocol.
//
// No C++ standard library containers: JSON lives in a stack Pool (see
// json.h) and strings are fixed char buffers.
//
// Keep this file pure ASCII.
#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/json.h"
#include "bootstrap.h"
#include "log.h"
#include "log_relay.h"

namespace protocol {
namespace {

typedef int (*NotifyPlatformFn)(const char* reqJson, char* respJson, int cap);

NotifyPlatformFn g_platform = nullptr;  // resolved NotifyPlatform, null when stopped
HostContext g_host = {};               // injected host capabilities

// Write a JSON body into the caller's buffer.
// Returns bytes written (excluding NUL), or -(need + 1) when the buffer is too
// small so the caller can grow it and retry.
int WriteResp(const char* body, char* resp, int cap) {
    if (body == nullptr || resp == nullptr || cap <= 0) {
        return -1;
    }
    const int need = static_cast<int>(std::strlen(body));
    if (need + 1 > cap) {
        return -(need + 1);
    }
    std::memcpy(resp, body, static_cast<size_t>(need));
    resp[need] = '\0';
    return need;
}

int RespOk(char* resp, int cap) {
    return WriteResp("{\"ok\":true}", resp, cap);
}

int RespErr(char* resp, int cap, int code, const char* msg) {
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value o = ppjson::MakeObj(pool);
    ppjson::ObjSetBool(pool, o, "ok", false);
    ppjson::ObjSetStr(pool, o, "err", (msg != nullptr) ? msg : "");
    ppjson::ObjSetNum(pool, o, "code", code);
    char buf[256] = {};
    o.Dump(buf, sizeof(buf));
    return WriteResp(buf, resp, cap);
}

// Pointer -> decimal string (pointer value as unsigned integer, into a caller
// buffer). Sent as a JSON string to avoid number precision loss on 64-bit.
void PtrToStr(const void* ptr, char* out, int cap) {
    std::snprintf(out, static_cast<size_t>(cap), "%llu",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr)));
}

// --- handlers for platform -> ghost commands --------------------------------

int HandleMainWindow(char* resp, int cap) {
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value o = ppjson::MakeObj(pool);
    char hwnd[32] = {};
    PtrToStr(g_host.mainWindow, hwnd, sizeof(hwnd));
    ppjson::ObjSetBool(pool, o, "ok", true);
    ppjson::ObjSetStr(pool, o, "hwnd", hwnd);
    char buf[128] = {};
    o.Dump(buf, sizeof(buf));
    return WriteResp(buf, resp, cap);
}

int HandleViewWindow(char* resp, int cap) {
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value o = ppjson::MakeObj(pool);
    char hwnd[32] = {};
    PtrToStr(g_host.viewWindow, hwnd, sizeof(hwnd));
    ppjson::ObjSetBool(pool, o, "ok", true);
    ppjson::ObjSetStr(pool, o, "hwnd", hwnd);
    char buf[128] = {};
    o.Dump(buf, sizeof(buf));
    return WriteResp(buf, resp, cap);
}

int HandleScale(char* resp, int cap) {
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value o = ppjson::MakeObj(pool);
    ppjson::ObjSetBool(pool, o, "ok", true);
    ppjson::ObjSetNum(pool, o, "scale", g_host.scale);
    char buf[128] = {};
    o.Dump(buf, sizeof(buf));
    return WriteResp(buf, resp, cap);
}

// 重新解析宿主窗口（主窗口 + FLUTTERVIEW 子窗口 + DPI）。
// 目标应用会重建窗口（换主题、开多窗口、分辨率变化），缓存的 HWND 就失效了；
// 编辑器点"刷新应用"时走这里，而不是继续拿旧句柄算几何。
int HandleRefreshWindow(char* resp, int cap) {
    // 只重选"已经失效"的句柄：窗口最小化时 IsWindow 仍为真，这时不能改主意去
    // 挑别的 Flutter 窗口（应用常同时开着图片查看器等窗口），否则会认错目标。
    bool ok = true;
    if (g_host.mainWindow == nullptr || IsWindow(g_host.mainWindow) == FALSE) {
        const HWND main = FindHostMainWindow();
        if (main != nullptr) {
            g_host.mainWindow = main;
            g_host.viewWindow = FindHostViewWindow(main);
            Log("protocol: refreshWindow re-resolved main=%p view=%p",
                reinterpret_cast<void*>(g_host.mainWindow),
                reinterpret_cast<void*>(g_host.viewWindow));
        } else {
            ok = false;
            Log("protocol: refreshWindow found no Flutter window");
        }
    } else if (g_host.viewWindow == nullptr || IsWindow(g_host.viewWindow) == FALSE) {
        g_host.viewWindow = FindHostViewWindow(g_host.mainWindow);
        Log("protocol: refreshWindow re-resolved view=%p",
            reinterpret_cast<void*>(g_host.viewWindow));
    }
    if (g_host.mainWindow != nullptr) {
        g_host.scale = HostDpiScale(g_host.mainWindow);
    }

    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value o = ppjson::MakeObj(pool);
    char mainStr[32] = {};
    char viewStr[32] = {};
    PtrToStr(g_host.mainWindow, mainStr, sizeof(mainStr));
    PtrToStr(g_host.viewWindow, viewStr, sizeof(viewStr));
    ppjson::ObjSetBool(pool, o, "ok", ok);
    ppjson::ObjSetStr(pool, o, "main", mainStr);
    ppjson::ObjSetStr(pool, o, "view", viewStr);
    ppjson::ObjSetNum(pool, o, "scale", g_host.scale);
    char buf[192] = {};
    o.Dump(buf, sizeof(buf));
    return WriteResp(buf, resp, cap);
}

int HandleGrabFrame(char* resp, int cap) {
    // Reserved: needs the D3D11 Present hook, not wired yet.
    return RespErr(resp, cap, 501, "grabFrame not implemented");
}

}  // namespace

int __cdecl NotifyGhost(const char* reqJson, char* respJson, int cap, void* ctx) {
    (void)ctx;  // host state lives in g_host; ctx is accepted for ABI symmetry
    if (reqJson == nullptr || respJson == nullptr || cap <= 0) {
        return -1;
    }
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    ppjson::Value req;
    char err[64] = {};
    if (!ppjson::Value::Parse(reqJson, pool, req, err, sizeof(err))) {
        return RespErr(respJson, cap, 400, "invalid json");
    }
    const char* cmd = req.Find("cmd").AsStr();

    if (std::strcmp(cmd, "mainWindow") == 0) return HandleMainWindow(respJson, cap);
    if (std::strcmp(cmd, "viewWindow") == 0) return HandleViewWindow(respJson, cap);
    if (std::strcmp(cmd, "scale") == 0)      return HandleScale(respJson, cap);
    if (std::strcmp(cmd, "refreshWindow") == 0) return HandleRefreshWindow(respJson, cap);
    if (std::strcmp(cmd, "grabFrame") == 0)  return HandleGrabFrame(respJson, cap);
    if (std::strcmp(cmd, "onUiThread") == 0) return RespErr(respJson, cap, 501, "onUiThread not implemented");

    // 模块 -> proxy.log：模块用 {"cmd":"log","tag":"console","text":"..."} 上报一行。
    if (std::strcmp(cmd, "log") == 0) {
        const char* text = req.Find("text").AsStr();
        const char* tag = req.Find("tag").AsStr();
        if (text[0] == '\0') {
            return RespErr(respJson, cap, 400, "empty text");
        }
        LogTagged((tag[0] != '\0') ? tag : "module", "%s", text);
        return RespOk(respJson, cap);
    }

    // 模块 -> proxy：注册日志 sink，之后 proxy 每写一行都会回调它。
    if (std::strcmp(cmd, "logSink") == 0) {
        const char* ptr = req.Find("ptr").AsStr();
        void* fn = reinterpret_cast<void*>(
            static_cast<uintptr_t>(std::strtoull(ptr, nullptr, 10)));
        if (!log_relay::SetSink(fn)) {
            return RespErr(respJson, cap, 400, "bad sink");
        }
        Log("protocol: log sink registered (%p)", fn);
        return RespOk(respJson, cap);
    }
    return RespErr(respJson, cap, 404, "unknown command");
}

int CallPlatform(const char* reqJson, char* respJson, int cap) {
    if (g_platform == nullptr) {
        return -1;
    }
    return g_platform(reqJson, respJson, cap);
}

namespace {

NotifyPlatformFn ResolveEntry(HMODULE module) {
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<NotifyPlatformFn>(GetProcAddress(module, "NotifyPlatform"));
}

// 发 init：把 NotifyGhost 指针交给模块，它之后用 {"cmd":...} 反向查询宿主能力。
// 回包是 best-effort，缺 init 回包不影响前面的调用是否成功。
void SendInit(const char* who, NotifyPlatformFn fn) {
    ppjson::Node poolNodes[32] = {};
    ppjson::Pool pool(poolNodes, 32);
    char dispatch[32] = {};
    PtrToStr(reinterpret_cast<const void*>(&NotifyGhost), dispatch, sizeof(dispatch));
    ppjson::Value init = ppjson::MakeObj(pool);
    ppjson::ObjSetStr(pool, init, "cmd", "init");
    ppjson::ObjSetStr(pool, init, "dispatch", dispatch);
    ppjson::ObjSetStr(pool, init, "ctx", "0");
    char req[512] = {};
    init.Dump(req, sizeof(req));

    char resp[4096] = {};
    const int n = fn(req, resp, static_cast<int>(sizeof(resp)));
    Log("protocol: %s init -> n=%d resp=%s", who, n, (n >= 0) ? resp : "(too large)");
}

void SendShutdown(const char* who, NotifyPlatformFn fn) {
    if (fn == nullptr) {
        return;
    }
    char resp[4096] = {};
    const int n = fn("{\"cmd\":\"shutdown\"}", resp, static_cast<int>(sizeof(resp)));
    Log("protocol: %s shutdown -> n=%d resp=%s", who, n, (n >= 0) ? resp : "(too large)");
}

}  // namespace

bool ProtocolStart(HMODULE frameworkDll, const HostContext& host) {
    if (frameworkDll == nullptr) {
        Log("protocol: no framework module");
        return false;
    }
    g_host = host;
    g_platform = ResolveEntry(frameworkDll);
    if (g_platform == nullptr) {
        Log("protocol: NotifyPlatform not found in framework.dll");
        return false;
    }
    SendInit("framework", g_platform);
    return true;
}

void ProtocolStop() {
    if (g_platform == nullptr) {
        return;
    }
    SendShutdown("framework", g_platform);
    g_platform = nullptr;
    g_host = HostContext{};
}

bool ModuleStart(HMODULE module, const HostContext& host) {
    (void)host;  // 额外模块只拿 dispatch 指针，宿主能力自己按需查
    const NotifyPlatformFn fn = ResolveEntry(module);
    if (fn == nullptr) {
        return false;
    }
    SendInit("module", fn);
    return true;
}

void ModuleStop(HMODULE module) {
    SendShutdown("module", ResolveEntry(module));
}

}  // namespace protocol