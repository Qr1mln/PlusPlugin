// config.cpp - version.ini reader/writer, Win32 + fixed buffers only.
//
// Keep this file pure ASCII.
#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cstring>
#include <cwctype>

#include "log.h"

namespace {

constexpr wchar_t kIniName[] = L"version.ini";
constexpr wchar_t kHostSection[] = L"host";
constexpr char kTargetKey[] = "target";
constexpr char kDebugKey[] = "debug";
constexpr char kDefaultTarget[] = "test.exe";

// version.ini is ASCII on purpose: the Win32 profile API reads it as ANSI.
// framework.dll 不走配置列表：bootstrap 固定加载同目录那一个并负责热加载；
// [modules] 只列额外的 JSON 模块（同样是 NotifyPlatform 入口）。
constexpr char kDefaultIniText[] =
    "; host process for version.dll, e.g. target=MyApp.exe\r\n"
    "[host]\r\n"
    "target=test.exe\r\n"
    "; pop a dialog when the proxy is attached (default true)\r\n"
    "debug=true\r\n"
    "\r\n"
    "; extra JSON modules in this directory, one plain dll name per line\r\n"
    "; (framework.dll is always loaded; uncomment to enable one)\r\n"
    "[modules]\r\n"
    "; console.dll\r\n";

constexpr DWORD kValueChars = 2048;   // profile API value buffer
constexpr int   kIniTextCap = 8192;    // whole-file scan buffer
constexpr int   kNameCap    = 128;     // one scanned key name

// --- small string helpers (no STL) -----------------------------------------

void CopyStr(char* dst, int cap, const char* src) {
    if (cap <= 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, static_cast<size_t>(cap - 1));
    dst[cap - 1] = '\0';
}

void CopyWide(wchar_t* dst, int cap, const wchar_t* src) {
    if (cap <= 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = L'\0';
        return;
    }
    wcsncpy(dst, src, static_cast<size_t>(cap - 1));
    dst[cap - 1] = L'\0';
}

void TrimInPlace(char* s) {
    char* first = s;
    while (*first == ' ' || *first == '\t' || *first == '"' || *first == '\'') {
        ++first;
    }
    const int len = static_cast<int>(std::strlen(first));
    int last = len;
    while (last > 0) {
        const char c = first[last - 1];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'') {
            --last;
        } else {
            break;
        }
    }
    first[last] = '\0';
    if (first != s) {
        std::memmove(s, first, static_cast<size_t>(last) + 1);
    }
}

// char -> wide (UTF-8). Empty input yields an empty result and true.
bool Widen(const char* utf8, wchar_t* out, int cap) {
    if (cap <= 0) {
        return false;
    }
    out[0] = L'\0';
    if (utf8 == nullptr || utf8[0] == '\0') {
        return true;
    }
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}

// --- Win32 file helpers -----------------------------------------------------

bool ReadFileAll(const wchar_t* path, char* out, int cap, int* outLen) {
    out[0] = '\0';
    if (outLen != nullptr) {
        *outLen = 0;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out, static_cast<DWORD>(cap - 1), &got, nullptr);
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    if (static_cast<int>(got) >= cap) {
        got = static_cast<DWORD>(cap - 1);
    }
    out[got] = '\0';
    if (outLen != nullptr) {
        *outLen = static_cast<int>(got);
    }
    return true;
}

bool WriteFileAll(const wchar_t* path, const char* data, int len) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
    return ok && static_cast<int>(written) == len;
}

// Iterate lines in-place. Ponytail returns one NUL-terminated line at a time
// and advances *cursor; trailing '\r' is stripped.
char* NextLine(char** cursor) {
    char* p = *cursor;
    if (p == nullptr || *p == '\0') {
        return nullptr;
    }
    char* line = p;
    while (*p != '\0' && *p != '\n') {
        ++p;
    }
    if (*p == '\n') {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = p;
    }
    const int len = static_cast<int>(std::strlen(line));
    if (len > 0 && line[len - 1] == '\r') {
        line[len - 1] = '\0';
    }
    return line;
}

// --- version.ini value reads ------------------------------------------------

bool ParseBool(const char* raw, bool fallback) {
    if (_stricmp(raw, "1") == 0 || _stricmp(raw, "true") == 0 ||
        _stricmp(raw, "yes") == 0 || _stricmp(raw, "on") == 0) {
        return true;
    }
    if (_stricmp(raw, "0") == 0 || _stricmp(raw, "false") == 0 ||
        _stricmp(raw, "no") == 0 || _stricmp(raw, "off") == 0) {
        return false;
    }
    return fallback;
}

// Section value via the Win32 profile API; false when absent or empty.
bool ReadProfileValue(const wchar_t* section, const char* key, char* out, int cap) {
    const wchar_t* ini = VersionIniPath();
    if (ini[0] == L'\0') {
        return false;
    }
    wchar_t wkey[64] = {};
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);

    wchar_t wvalue[kValueChars] = {};
    const DWORD n = GetPrivateProfileStringW(section, wkey, L"", wvalue, kValueChars, ini);
    if (n == 0 || wvalue[0] == 0) {
        return false;
    }
    char utf8[kValueChars * 2] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, utf8, sizeof utf8, nullptr, nullptr) <= 0) {
        return false;
    }
    CopyStr(out, cap, utf8);
    return true;
}

// Fallback for bare "key=value" files written before the [host] header existed.
bool ReadLegacyValue(const char* key, char* out, int cap) {
    const wchar_t* ini = VersionIniPath();
    if (ini[0] == L'\0') {
        return false;
    }
    char buf[kIniTextCap] = {};
    if (!ReadFileAll(ini, buf, kIniTextCap, nullptr)) {
        return false;
    }
    char* cursor = buf;
    for (char* line = NextLine(&cursor); line != nullptr; line = NextLine(&cursor)) {
        char* first = line;
        while (*first == ' ' || *first == '\t') {
            ++first;
        }
        if (*first == '\0') {
            continue;
        }
        const char head = *first;
        if (head == ';' || head == '#' || head == '[') {
            continue;
        }
        char* eq = std::strchr(first, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char name[kNameCap] = {};
        CopyStr(name, kNameCap, first);
        TrimInPlace(name);
        if (_stricmp(name, key) != 0) {
            continue;
        }
        char* value = eq + 1;
        char* comment = std::strchr(value, ';');
        if (comment != nullptr) {
            *comment = '\0';
        }
        TrimInPlace(value);
        if (*value == '\0') {
            return false;
        }
        CopyStr(out, cap, value);
        return true;
    }
    return false;
}

bool ReadHostIniValue(const char* key, char* out, int cap) {
    return ReadProfileValue(kHostSection, key, out, cap) || ReadLegacyValue(key, out, cap);
}

// --- writing / existence ----------------------------------------------------

bool WriteDefaultIni() {
    const wchar_t* ini = VersionIniPath();
    if (ini[0] == L'\0') {
        return false;
    }
    return WriteFileAll(ini, kDefaultIniText, static_cast<int>(std::strlen(kDefaultIniText)));
}

void EnsureIniExists() {
    static const bool done = [] {
        const wchar_t* ini = VersionIniPath();
        if (ini[0] == L'\0') {
            return false;
        }
        if (GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
        if (WriteDefaultIni()) {
            Log("%ls not found, created with target=%s", kIniName, kDefaultTarget);
            return true;
        }
        Log("WARN: %ls not found and could not be created", kIniName);
        return false;
    }();
    (void)done;
}

// [modules] 的一行必须是一个裸 dll 名：带路径分隔符/.. 的一律拒绝，免得
// version.ini 变成任意 DLL 加载器。
bool IsPlainDllName(const char* name) {
    if (name == nullptr) {
        return false;
    }
    const size_t len = std::strlen(name);
    if (len <= 4 || len >= static_cast<size_t>(kMaxModuleNameChars)) {
        return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
        if (std::strchr("\\/:*?\"<>|", *p) != nullptr) {
            return false;
        }
    }
    return _stricmp(name + len - 4, ".dll") == 0;
}

}  // namespace


// --- path resolution --------------------------------------------------------
// Defined at namespace scope (declared in config.h) so the whole translation
// unit, and the rest of the proxy, share one definition.

const wchar_t* ModuleDir() {
    static wchar_t dir[kConfigDirChars] = {};
    static const bool init = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
        if (self == nullptr) {
            return true;
        }
        wchar_t buf[kConfigDirChars] = {};
        if (GetModuleFileNameW(self, buf, kConfigDirChars) == 0) {
            return true;
        }
        wchar_t* slash = wcsrchr(buf, L'\\');
        if (slash == nullptr) {
            return true;
        }
        *slash = L'\0';
        CopyWide(dir, kConfigDirChars, buf);
        return true;
    }();
    (void)init;
    return dir;
}

const wchar_t* VersionIniPath() {
    static wchar_t path[kConfigDirChars] = {};
    static const bool init = [] {
        const wchar_t* dir = ModuleDir();
        if (dir[0] == L'\0') {
            return true;
        }
        swprintf_s(path, kConfigDirChars, L"%s\\%s", dir, kIniName);
        return true;
    }();
    (void)init;
    return path;
}

bool DebugEnabled() {
    static const bool enabled = [] {
        EnsureIniExists();
        char raw[kNameCap] = {};
        if (!ReadHostIniValue(kDebugKey, raw, kNameCap)) {
            return true;   // missing key / empty value: on by default
        }
        return ParseBool(raw, true);
    }();
    return enabled;
}

const wchar_t* TargetExeName() {
    static wchar_t cached[256] = {};
    static const bool init = [] {
        if (VersionIniPath()[0] == L'\0') {
            return true;   // cannot resolve our own directory
        }
        EnsureIniExists();
        char raw[512] = {};
        if (ReadHostIniValue(kTargetKey, raw, sizeof raw)) {
            wchar_t wide[256] = {};
            if (Widen(raw, wide, 256) && wide[0] != L'\0') {
                CopyWide(cached, 256, wide);
                return true;
            }
        }
        Log("WARN: %ls has no usable target=, using %s", kIniName, kDefaultTarget);
        Widen(kDefaultTarget, cached, 256);
        return true;
    }();
    (void)init;
    return cached;
}

int ExtraModuleNames(wchar_t out[][kMaxModuleNameChars], int cap) {
    static wchar_t cached[kMaxModuleNames][kMaxModuleNameChars] = {};
    static int cachedCount = -1;
    if (cachedCount < 0) {
        cachedCount = 0;
        EnsureIniExists();
        const wchar_t* ini = VersionIniPath();
        char buf[kIniTextCap] = {};
        if (ini[0] != L'\0' && ReadFileAll(ini, buf, kIniTextCap, nullptr)) {
            bool inSection = false;
            char* cursor = buf;
            for (char* line = NextLine(&cursor); line != nullptr;
                 line = NextLine(&cursor)) {
                char* first = line;
                while (*first == ' ' || *first == '\t') {
                    ++first;
                }
                if (*first == '\0' || *first == ';' || *first == '#') {
                    continue;
                }
                if (*first == '[') {
                    char* close = std::strchr(first, ']');
                    if (close != nullptr) {
                        *close = '\0';
                        inSection = (_stricmp(first + 1, "modules") == 0);
                    }
                    continue;
                }
                if (!inSection) {
                    continue;
                }
                char* end = first + std::strlen(first);
                while (end > first && (end[-1] == ' ' || end[-1] == '\t')) {
                    --end;
                }
                *end = '\0';
                if (!IsPlainDllName(first)) {
                    Log("WARN: %ls [modules] entry rejected (bare name ending in .dll): %s",
                        kIniName, first);
                    continue;
                }
                bool dup = false;
                for (int i = 0; i < cachedCount; ++i) {
                    char existing[kMaxModuleNameChars] = {};
                    WideCharToMultiByte(CP_ACP, 0, cached[i], -1, existing,
                                        sizeof existing, nullptr, nullptr);
                    if (_stricmp(existing, first) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                if (!Widen(first, cached[cachedCount], kMaxModuleNameChars)) {
                    continue;
                }
                ++cachedCount;
                if (cachedCount >= kMaxModuleNames) {
                    break;
                }
            }
            if (cachedCount > 0) {
                Log("%ls [modules]: %d extra module(s)", kIniName, cachedCount);
            }
        }
    }
    if (cap <= 0) {
        return 0;
    }
    const int n = (cachedCount < cap) ? cachedCount : cap;
    for (int i = 0; i < n; ++i) {
        CopyWide(out[i], kMaxModuleNameChars, cached[i]);
    }
    return n;
}
