// test_cdp.cpp - exercises the engine's CDP section against a live endpoint.
//
// The version.dll proxy arms --remote-debugging-port for WebView2 hosts; this
// harness verifies the engine's client (connect / query / dispatch) against any
// Chromium with the same port, e.g. a local Edge:
//
//   msedge --headless=new --remote-debugging-port=9223 \
//          --user-data-dir=%TEMP%\cdp-test-profile \
//          file:///D:/PyProjects/PlusPlugin/.cdp_test/cdp_test.html
//
// The page records every event with its isTrusted flag, so a pass means the
// clicks really entered the trusted input pipeline, not just that the calls
// returned. Build & run (from plugin/script, in a VS x64 prompt):
//
//   cl /nologo /MT /W4 /utf-8 test_cdp.cpp /link winhttp.lib user32.lib
//   test_cdp.exe
//
// Keep this file pure ASCII.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- shims: what script_plugin.cpp normally hands to script_engine.inc ----

static HWND  g_hwnd = nullptr;
static HWND  g_viewHwnd = nullptr;
static float g_scale = 1.0f;

static void LogFromOverlay(const char* text) {
    printf("    %s\n", text);
}

static void SetScriptStatus(int code, const char* reason) {
    (void)code;
    (void)reason;
}

#include "script_engine.inc"

static int CheckHits(const char* expectType) {
    char out[1024] = {};
    if (!CdpEvaluate("JSON.stringify(window.hits)", out, sizeof(out))) {
        printf("MAIN: hits evaluate failed\n");
        return 1;
    }
    printf("MAIN: hits=%s\n", out);
    if (strstr(out, expectType) == nullptr) {
        printf("MAIN: FAIL, no \"%s\" event\n", expectType);
        return 1;
    }
    if (strstr(out, "\"trusted\":true") == nullptr &&
        strstr(out, "\"t\":true") == nullptr) {
        printf("MAIN: FAIL, no trusted event\n");
        return 1;
    }
    return 0;
}

static int DiagUpgrade() {
    // Step-by-step upgrade trace: which WinHTTP call fails and with what.
    g_cdpSession = WinHttpOpen(L"pplugin-diag", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (g_cdpSession == nullptr) { printf("diag: open err=%lu\n", GetLastError()); return 1; }
    WinHttpSetTimeouts(g_cdpSession, 3000, 3000, 3000, 4000);
    g_cdpConnect = WinHttpConnect(g_cdpSession, L"127.0.0.1", kCdpPort, 0);
    if (g_cdpConnect == nullptr) { printf("diag: connect err=%lu\n", GetLastError()); return 1; }

    wchar_t wsPath[128] = {};
    char url[256] = {};
    if (!CdpPickTarget(nullptr, wsPath, 128, url, sizeof(url))) {
        printf("diag: pick target failed\n");
        return 1;
    }
    wprintf(L"diag: wsPath=%s\n", wsPath);

    HINTERNET req = WinHttpOpenRequest(g_cdpConnect, L"GET", wsPath, nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (req == nullptr) { printf("diag: openrequest err=%lu\n", GetLastError()); return 1; }

    BOOL up = TRUE;
    SetLastError(0);
    BOOL r1 = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                               &up, sizeof(up));
    printf("diag: setopt BOOL(4)=%d err=%lu\n", r1, GetLastError());
    SetLastError(0);
    BOOL r1b = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                                nullptr, 0);
    printf("diag: setopt(NULL,0)=%d err=%lu\n", r1b, GetLastError());
    SetLastError(0);
    BOOL r2 = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    printf("diag: send=%d err=%lu\n", r2, GetLastError());
    SetLastError(0);
    BOOL r3 = WinHttpReceiveResponse(req, nullptr);
    printf("diag: recv=%d err=%lu\n", r3, GetLastError());
    DWORD st = 0, sz = sizeof(st);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &st, &sz, nullptr);
    printf("diag: status=%lu\n", st);
    SetLastError(0);
    HINTERNET ws = (r1 && r2 && r3) ? WinHttpWebSocketCompleteUpgrade(req, 0) : nullptr;
    printf("diag: ws=%p err=%lu\n", ws, GetLastError());
    WinHttpCloseHandle(req);
    return 0;
}

static int DiagWs() {
    CdpRunBegin();
    if (!CdpEnsure(nullptr)) return 1;
    const char* msg =
        "{\"id\":900,\"method\":\"Runtime.evaluate\","
        "\"params\":{\"expression\":\"'abc'\"}}";
    printf("diag: send=%d\n", CdpSendText(msg) ? 1 : 0);
    for (int i = 0; i < 6; ++i) {
        char buf[4096] = {};
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE t = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        DWORD r = WinHttpWebSocketReceive(g_cdpWs, buf, sizeof(buf) - 1, &read, &t);
        buf[read] = 0;
        printf("diag: recv ret=%lu type=%d read=%lu %.100s\n", r, (int)t, read, buf);
        if (r != 0) break;
        if (t == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "diag") == 0) {
        return DiagUpgrade();
    }
    if (argc > 1 && strcmp(argv[1], "ws") == 0) {
        return DiagWs();
    }
    CdpRunBegin();

    if (!CdpEnsure(nullptr)) {
        printf("MAIN: FAIL, no connect\n");
        return 1;
    }

    char probe[256] = {};
    bool evok = CdpEvaluate("JSON.stringify(!!document.querySelector('#btn'))",
                            probe, sizeof(probe));
    printf("MAIN: evaluate probe ok=%d value=%s\n", evok, probe);

    char exprOut[256] = {};
    char selQ[512] = {};
    CdpJsonQuote("#btn", selQ, sizeof(selQ));
    char expr[1024] = {};
    sprintf_s(expr, sizeof(expr),
        "(function(){var e=document.querySelector(%s);"
        "if(!e)return '{\"found\":0}';"
        "e.scrollIntoView({block:'nearest',inline:'nearest'});"
        "var r=e.getBoundingClientRect();"
        "return JSON.stringify({found:1,x:r.left+r.width/2,y:r.top+r.height/2,"
        "w:r.width,h:r.height,vw:innerWidth,vh:innerHeight});})()",
        selQ);
    bool evok2 = CdpEvaluate(expr, exprOut, sizeof(exprOut));
    printf("MAIN: expr eval ok=%d value=%s\n", evok2, exprOut);

    CdpRect rc = {};
    if (!CdpQuerySelector("#btn", &rc) || rc.found == 0) {
        printf("MAIN: FAIL, #btn not found\n");
        return 1;
    }
    printf("MAIN: #btn center %.0f,%.0f size %.0fx%.0f\n", rc.x, rc.y, rc.w, rc.h);
    if (rc.x < 250.0f || rc.x > 270.0f || rc.y < 170.0f || rc.y > 190.0f) {
        printf("MAIN: FAIL, unexpected rect\n");
        return 1;
    }

    CdpRect none = {};
    if (!CdpQuerySelector("#nope", &none) || none.found != 0) {
        printf("MAIN: FAIL, #nope should be found=0\n");
        return 1;
    }

    char clear[8] = {};
    // Full engine path: selector event -> CDP click (no fallback wanted).
    // The trailing 'ok' keeps the evaluate result a string, like CdpEvaluate wants.
    CdpEvaluate("window.hits=[];'ok'", clear, sizeof(clear));
    ScriptEvent ev = ScriptEvent();
    strcpy_s(ev.selector, "#btn");
    ev.click = 0;
    RunEventActions(ev, "");
    Sleep(400);
    if (CheckHits("click") != 0) return 1;

    CdpEvaluate("window.hits=[];'ok'", clear, sizeof(clear));
    ev.click = 1;
    RunEventActions(ev, "");
    Sleep(400);
    if (CheckHits("dblclick") != 0) return 1;

    CdpEvaluate("window.hits=[];'ok'", clear, sizeof(clear));
    ev.click = 2;
    RunEventActions(ev, "");
    Sleep(400);
    if (CheckHits("contextmenu") != 0) return 1;

    CdpShutdown();
    printf("MAIN: CDP_TEST_OK\n");
    return 0;
}
