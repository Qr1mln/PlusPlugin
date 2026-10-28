// protocol.cpp - implementation of the ghost side of the version protocol.
//
// No C++ standard library containers: JSON lives in a stack Pool (see
// json_util.h) and strings are fixed char buffers.
//
// Keep this file pure ASCII.
#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/json_util.h"
#include "log.h"

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

int HandlePublishLog(ppjson::Value req, char* resp, int cap) {
    ppjson::Value text = req.Find("text");
    LogTagged("platform", "%s", text.AsStr());
    return RespOk(resp, cap);
}

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

    if (std::strcmp(cmd, "publishLog") == 0) return HandlePublishLog(req, respJson, cap);
    if (std::strcmp(cmd, "mainWindow") == 0) return HandleMainWindow(respJson, cap);
    if (std::strcmp(cmd, "viewWindow") == 0) return HandleViewWindow(respJson, cap);
    if (std::strcmp(cmd, "scale") == 0)      return HandleScale(respJson, cap);
    if (std::strcmp(cmd, "grabFrame") == 0)  return HandleGrabFrame(respJson, cap);
    if (std::strcmp(cmd, "onUiThread") == 0) return RespErr(respJson, cap, 501, "onUiThread not implemented");
    return RespErr(respJson, cap, 404, "unknown command");
}

int CallPlatform(const char* reqJson, char* respJson, int cap) {
    if (g_platform == nullptr) {
        return -1;
    }
    return g_platform(reqJson, respJson, cap);
}

bool ProtocolStart(HMODULE frameworkDll, const HostContext& host) {
    if (frameworkDll == nullptr) {
        Log("protocol: no framework module");
        return false;
    }
    g_host = host;
    g_platform = reinterpret_cast<NotifyPlatformFn>(GetProcAddress(frameworkDll, "NotifyPlatform"));
    if (g_platform == nullptr) {
        Log("protocol: NotifyPlatform not found in framework.dll");
        return false;
    }

    // Hand platform its reverse-call pointer. The reply is best-effort: a
    // missing init reply does not break the forward path.
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
    const int n = g_platform(req, resp, static_cast<int>(sizeof(resp)));
    Log("protocol: init -> n=%d resp=%s", n, (n >= 0) ? resp : "(too large)");
    return true;
}

void ProtocolStop() {
    if (g_platform == nullptr) {
        return;
    }
    char resp[4096] = {};
    const int n = g_platform("{\"cmd\":\"shutdown\"}", resp, static_cast<int>(sizeof(resp)));
    Log("protocol: shutdown -> n=%d resp=%s", n, (n >= 0) ? resp : "(too large)");
    g_platform = nullptr;
    g_host = HostContext{};
}

}  // namespace protocol