// config.cpp - version.ini reader/writer, Win32 + fixed buffers only.
//
// Keep this file pure ASCII.
#include "config.h"

#include <windows.h>

#include <cwchar>
#include <cstring>
#include <cwctype>

#include "log.h"

namespace {

constexpr wchar_t kIniName[] = L"version.ini";
constexpr wchar_t kHostSection[] = L"host";
constexpr wchar_t kLoaderSection[] = L"loaders";
constexpr char kTargetKey[] = "target";
constexpr char kDebugKey[] = "debug";
constexpr char kDefaultTarget[] = "test.exe";
constexpr char kDefaultDll[] = "framework.dll";

// version.ini is ASCII on purpose: the Win32 profile API reads it as ANSI.
constexpr char kDefaultIniText[] =
    "; host process for version.dll, e.g. target=MyApp.exe\r\n"
    "[host]\r\n"
    "target=test.exe\r\n"
    "; pop a dialog when the proxy is attached (default true)\r\n"
    "debug=true\r\n"
    "\r\n"
    "; DLLs loaded from this directory, one plain file name per line.\r\n"
    "; an entry is loaded only while it is listed here\r\n"
    "[loaders]\r\n"
    "framework.dll\r\n";

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

void TrimWideInPlace(wchar_t* s) {
    wchar_t* first = s;
    while (*first == L' ' || *first == L'\t') {
        ++first;
    }
    const int len = static_cast<int>(wcslen(first));
    int last = len;
    while (last > 0 && (first[last - 1] == L' ' || first[last - 1] == L'\t')) {
        --last;
    }
    first[last] = L'\0';
    if (first != s) {
        std::memmove(s, first, (static_cast<size_t>(last) + 1) * sizeof(wchar_t));
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

// --- path resolution --------------------------------------------------------

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
            Log("%ls not found, created with target=%s, [loaders] %s", kIniName,
                kDefaultTarget, kDefaultDll);
            return true;
        }
        Log("WARN: %ls not found and could not be created", kIniName);
        return false;
    }();
    (void)done;
}

// --- [loaders] scanning -----------------------------------------------------

// A [loaders] entry must be a bare file name inside ModuleDir(). Rejecting path
// separators, ".." and absolute paths keeps the key from turning the proxy into
// an arbitrary-DLL loader for anyone who can write version.ini.
bool IsPlainDllName(const wchar_t* name) {
    if (name == nullptr || name[0] == L'\0') {
        return false;
    }
    const int len = static_cast<int>(wcslen(name));
    if (len > kMaxDllNameChars || len <= 4) {
        return false;
    }
    if (wcsstr(name, L"..") != nullptr) {
        return false;
    }
    for (const wchar_t* p = name; *p != L'\0'; ++p) {
        const wchar_t c = *p;
        if (c == L'\\' || c == L'/' || c == L':' || c == L'"' || c == L'*' ||
            c == L'?' || c == L'<' || c == L'>' || c == L'|') {
            return false;
        }
    }
    const wchar_t* tail = name + (len - 4);
    return _wcsicmp(tail, L".dll") == 0;
}

bool ListContains(wchar_t out[][kMaxDllNameChars], int count, const wchar_t* name) {
    for (int i = 0; i < count; ++i) {
        if (_wcsicmp(out[i], name) == 0) {
            return true;
        }
    }
    return false;
}

// Bare (keyless) lines under [loaders], each a plain DLL name.
int ReadLoaderSectionLines(wchar_t out[][kMaxDllNameChars], int cap) {
    const wchar_t* ini = VersionIniPath();
    if (ini[0] == L'\0') {
        return 0;
    }
    char buf[kIniTextCap] = {};
    if (!ReadFileAll(ini, buf, kIniTextCap, nullptr)) {
        return 0;
    }
    int count = 0;
    bool inLoader = false;
    char* cursor = buf;
    for (char* line = NextLine(&cursor); line != nullptr; line = NextLine(&cursor)) {
        char* first = line;
        while (*first == ' ' || *first == '\t') {
            ++first;
        }
        if (*first == '\0') {
            continue;
        }
        if (*first == '[') {
            char* close = std::strchr(first, ']');
            if (close != nullptr) {
                *close = '\0';
                char sec[kNameCap] = {};
                CopyStr(sec, kNameCap, first + 1);
                TrimInPlace(sec);
                wchar_t wsec[kNameCap] = {};
                Widen(sec, wsec, kNameCap);
                inLoader = (_wcsicmp(wsec, kLoaderSection) == 0);
            }
            continue;
        }
        const char head = *first;
        if (head == ';' || head == '#' || !inLoader) {
            continue;
        }
        if (std::strchr(first, '=') != nullptr) {
            continue;   // "dlls=a,b" form is handled by LoadDllNames' caller
        }
        wchar_t name[kMaxDllNameChars] = {};
        if (!Widen(first, name, kMaxDllNameChars)) {
            continue;
        }
        TrimWideInPlace(name);
        if (!IsPlainDllName(name) || ListContains(out, count, name)) {
            continue;
        }
        CopyWide(out[count], kMaxDllNameChars, name);
        ++count;
        if (count >= cap) {
            break;
        }
    }
    return count;
}

}  // namespace

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

int LoadDllNames(wchar_t out[][kMaxDllNameChars], int cap) {
    static wchar_t cached[kMaxLoadDlls][kMaxDllNameChars] = {};
    static int cachedCount = -1;
    if (cachedCount < 0) {
        EnsureIniExists();
        cachedCount = ReadLoaderSectionLines(cached, kMaxLoadDlls);
        if (cachedCount == 0) {
            Log("WARN: %ls has no usable [loaders] entries, loading nothing", kIniName);
        }
    }
    if (cap <= 0) {
        return 0;
    }
    const int n = (cachedCount < cap) ? cachedCount : cap;
    for (int i = 0; i < n; ++i) {
        CopyWide(out[i], kMaxDllNameChars, cached[i]);
    }
    return n;
}