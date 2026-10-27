#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "config.h"

void (*g_logFanout)(const char* line) = nullptr;

namespace {

constexpr int kLogLineChars = 1024;

// Serializes appends: several threads log at once (loader, plugin workers), and
// a line must never interleave with another one.
std::mutex g_logMx;

// proxy.log next to our own DLL (travels with the artifact); %TEMP% when the
// module path cannot be resolved.
std::filesystem::path LogPath() {
    const std::filesystem::path& dir = ModuleDir();
    if (!dir.empty()) {
        return dir / L"proxy.log";
    }
    wchar_t tmp[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        return std::filesystem::path(L"proxy.log");
    }
    return std::filesystem::path(tmp) / L"proxy.log";
}

}  // namespace

namespace {

void LogV(const char* component, const char* fmt, va_list ap) {
    char line[kLogLineChars] = {};
    vsnprintf(line, sizeof line, fmt, ap);

    // Resolved once: the DLL never moves at runtime.
    static const std::filesystem::path path = LogPath();

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[96] = {};
    snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u]-[%s] ",
             static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
             static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds),
             (component != nullptr && component[0] != 0) ? component : "proxy");

    // One record, one write, under a lock: no torn or interleaved lines.
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        std::ofstream out(path, std::ios::app | std::ios::binary);
        if (out) {
            std::string record;
            record.reserve(160);
            record.append(stamp);
            record.append(line);
            record.push_back('\n');
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
        }
    }

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
