// protocol.cpp - implementation of the base side of the version protocol.
//
// Keep this file pure ASCII.
#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "json_util.h"
#include "log.h"

namespace protocol {
namespace {

typedef int (*PlatformEntryFn)(const char* reqJson, char* respJson, int cap);

PlatformEntryFn g_platform = nullptr;  // resolved PlatformEntry, null when stopped
HostContext g_host = {};               // injected host capabilities

// Write a JSON body into the caller's buffer.
// Returns bytes written (excluding NUL), or -(need + 1) when the buffer is too
// small so the caller can grow it and retry.
int WriteResp(const std::string& body, char* resp, int cap) {
    const int need = static_cast<int>(body.size());
    if (need + 1 > cap) {
        return -(need + 1);
    }
    std::memcpy(resp, body.data(), body.size());
    resp[need] = '\0';
    return need;
}

int RespOk(char* resp, int cap) {
    return WriteResp("{\"ok\":true}", resp, cap);
}

int RespErr(char* resp, int cap, int code, const char* msg) {
    ppjson::Value o = ppjson::Value::Obj();
    o.Set("ok", ppjson::Value::Bool(false));
    o.Set("err", ppjson::Value::Str(msg != nullptr ? msg : ""));
    o.Set("code", ppjson::Value::Num(code));
    return WriteResp(o.Dump(), resp, cap);
}

// Pointer -> decimal string. Sent as a JSON string to avoid number precision
// loss on 64-bit addresses.
std::string PtrToString(const void* ptr) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr)));
    return std::string(buf);
}

// --- handlers for platform -> base commands --------------------------------

int HandlePublishLog(const ppjson::Value& req, char* resp, int cap) {
    const ppjson::Value* text = req.Find("text");
    LogTagged("platform", "%s", (text != nullptr) ? text->AsStr().c_str() : "");
    return RespOk(resp, cap);
}

int HandleMainWindow(char* resp, int cap) {
    ppjson::Value o = ppjson::Value::Obj();
    o.Set("ok", ppjson::Value::Bool(true));
    o.Set("hwnd", ppjson::Value::Str(PtrToString(g_host.mainWindow)));
    return WriteResp(o.Dump(), resp, cap);
}

int HandleViewWindow(char* resp, int cap) {
    ppjson::Value o = ppjson::Value::Obj();
    o.Set("ok", ppjson::Value::Bool(true));
    o.Set("hwnd", ppjson::Value::Str(PtrToString(g_host.viewWindow)));
    return WriteResp(o.Dump(), resp, cap);
}

int HandleScale(char* resp, int cap) {
    ppjson::Value o = ppjson::Value::Obj();
    o.Set("ok", ppjson::Value::Bool(true));
    o.Set("scale", ppjson::Value::Num(g_host.scale));
    return WriteResp(o.Dump(), resp, cap);
}

int HandleGrabFrame(char* resp, int cap) {
    // Reserved: needs the D3D11 Present hook, not wired yet.
    return RespErr(resp, cap, 501, "grabFrame not implemented");
}

}  // namespace

int __cdecl BaseDispatch(const char* reqJson, char* respJson, int cap, void* ctx) {
    (void)ctx;  // host state lives in g_host; ctx is accepted for ABI symmetry
    if (reqJson == nullptr || respJson == nullptr || cap <= 0) {
        return -1;
    }
    std::string err;
    ppjson::Value req;
    if (!ppjson::Value::Parse(reqJson, req, &err)) {
        return RespErr(respJson, cap, 400, "invalid json");
    }
    const ppjson::Value* cmdField = req.Find("cmd");
    const std::string cmd = (cmdField != nullptr) ? cmdField->AsStr() : std::string();

    if (cmd == "publishLog") return HandlePublishLog(req, respJson, cap);
    if (cmd == "mainWindow") return HandleMainWindow(respJson, cap);
    if (cmd == "viewWindow") return HandleViewWindow(respJson, cap);
    if (cmd == "scale")      return HandleScale(respJson, cap);
    if (cmd == "grabFrame")  return HandleGrabFrame(respJson, cap);
    if (cmd == "onUiThread") return RespErr(respJson, cap, 501, "onUiThread not implemented");
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
    g_platform = reinterpret_cast<PlatformEntryFn>(GetProcAddress(frameworkDll, "PlatformEntry"));
    if (g_platform == nullptr) {
        Log("protocol: PlatformEntry not found in framework.dll");
        return false;
    }

    // Hand platform its reverse-call pointer. The reply is best-effort: a
    // missing init reply does not break the forward path.
    ppjson::Value init = ppjson::Value::Obj();
    init.Set("cmd", ppjson::Value::Str("init"));
    init.Set("dispatch",
             ppjson::Value::Str(PtrToString(reinterpret_cast<const void*>(&BaseDispatch))));
    init.Set("ctx", ppjson::Value::Str("0"));

    char resp[4096] = {};
    const int n = g_platform(init.Dump().c_str(), resp, static_cast<int>(sizeof(resp)));
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