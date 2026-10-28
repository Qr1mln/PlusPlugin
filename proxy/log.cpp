// log.cpp - single-line log writer, Win32 + fixed buffers only.
//
// Keep this file pure ASCII.
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "config.h"

void (*g_logFanout)(const char* line) = nullptr;

namespace {

constexpr int kLogLineChars = 1024;
constexpr int kLogPathChars = 512;   // MAX_PATH-sized

// Exclusive lock: several threads log at once and a line must never interleave.
// SRWLOCK needs no explicit init, unlike CRITICAL_SECTION.
SRWLOCK g_logLock = SRWLOCK_INIT;

// proxy.log next to our own DLL (travels with the artifact); %TEMP% fallback.
const wchar_t* LogPath() {
    static wchar_t path[kLogPathChars] = {};
    static const bool init = [] {
        const wchar_t* dir = ModuleDir();
        if (dir[0] != L'\0') {
            swprintf_s(path, kLogPathChars, L"%s\\proxy.log", dir);
            return true;
        }
        wchar_t tmp[kLogPathChars] = {};
        const DWORD n = GetTempPathW(kLogPathChars, tmp);
        if (n == 0 || n >= static_cast<DWORD>(kLogPathChars)) {
            wcscpy_s(path, kLogPathChars, L"proxy.log");
            return true;
        }
        swprintf_s(path, kLogPathChars, L"%sproxy.log", tmp);
        return true;
    }();
    (void)init;
    return path;
}

void LogV(const char* component, const char* fmt, va_list ap) {
    char line[kLogLineChars] = {};
    vsnprintf(line, sizeof line, fmt, ap);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[128] = {};
    snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u]-[%s] ",
             static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
             static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds),
             (component != nullptr && component[0] != 0) ? component : "proxy");

    // One record, one write, under a lock: no torn or interleaved lines.
    char record[kLogLineChars + 128] = {};
    snprintf(record, sizeof record, "%s%s\n", stamp, line);
    const int recordLen = static_cast<int>(std::strlen(record));

    AcquireSRWLockExclusive(&g_logLock);
    HANDLE h = CreateFileW(LogPath(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(h, record, static_cast<DWORD>(recordLen), &written, nullptr);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_logLock);

    // Mirror to plugins. The fan-out guards against re-entry, so a sink can
    // never recurse into Log().
    if (g_logFanout != nullptr) {
        g_logFanout(line);
    }
}

}  // namespace

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV("proxy", fmt, ap);
    va_end(ap);
}

void LogTagged(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(component, fmt, ap);
    va_end(ap);
}