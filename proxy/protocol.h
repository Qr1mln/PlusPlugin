// protocol.h - base side of the version protocol (version.dll <-> framework.dll).
//
// Contract (see docs/framework_protocol.md):
//   base -> platform : int PlatformEntry(req, resp, cap)      (platform exports)
//   platform -> base : int BaseDispatch(req, resp, cap, ctx)  (base provides)
//
// Both signatures never change; all evolution happens inside JSON. The request
// and response buffers are owned by the caller, so neither side frees memory
// across heaps (C++ CRT vs. C# native allocator).
//
// Keep this file pure ASCII.
#pragma once

#include <windows.h>

namespace protocol {

// Host capabilities injected by base (bootstrap). A plain struct so the
// protocol module does not depend on the plugins namespace internals.
struct HostContext {
    HWND  mainWindow = nullptr;  // Flutter top-level window
    HWND  viewWindow = nullptr;  // FLUTTERVIEW child (input target); may be null
    float scale = 1.0f;          // DPI scale derived from the host window
};

// base -> platform reverse call, handed to platform through the "init" request.
// Never change the signature.
int __cdecl BaseDispatch(const char* reqJson, char* respJson, int cap, void* ctx);

// base -> platform forward call. Returns >= 0 bytes written, or < 0 the number
// of bytes required (the caller grows its buffer and retries).
int CallPlatform(const char* reqJson, char* respJson, int cap);

// Lifecycle. Resolves PlatformEntry inside frameworkDll, then sends "init".
bool ProtocolStart(HMODULE frameworkDll, const HostContext& host);

// Sends "shutdown" and drops the platform pointer. Call before FreeLibrary.
void ProtocolStop();

}  // namespace protocol