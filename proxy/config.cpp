#include "config.h"

#include <windows.h>

#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#include "log.h"

namespace {

constexpr wchar_t kIniName[] = L"version.ini";
constexpr wchar_t kHostSection[] = L"host";
constexpr wchar_t kLoaderSection[] = L"loaders";
constexpr char kTargetKey[] = "target";
constexpr char kDebugKey[] = "debug";
constexpr char kDefaultTarget[] = "test.exe";
constexpr char kDefaultDll[] = "framework.dll";

// [loaders] limits. The list is a configuration file, not a plugin directory:
// keep it short so a typo cannot ask for an unbounded number of modules.
constexpr size_t kMaxLoadDlls = 8;
constexpr size_t kMaxDllNameChars = 64;

// version.ini is ASCII on purpose: the Win32 profile API reads it as ANSI, so
// keep keys and values ASCII and stick to ';' comments.
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

// Wide buffer sizes: [loaders] dlls= is the only key that can get long, so one
// size covers every key. A short buffer would silently truncate the tail of
// the list and drop modules without any error.
constexpr DWORD kValueChars = 2048;
constexpr size_t kUtf8Bytes = 8192;

std::filesystem::path SelfDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfDir), &self);
    if (self == nullptr) {
        return {};
    }
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(self, buf, MAX_PATH) == 0) {
        return {};
    }
    return std::filesystem::path(buf).parent_path();
}

std::wstring Widen(const char* utf8) {
    if (utf8 == nullptr || utf8[0] == 0) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], len);
    return out;
}

std::string Trim(std::string text) {
    const char* blanks = " \t\"'";
    const size_t first = text.find_first_not_of(blanks);
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

std::wstring TrimWide(std::wstring text) {
    const wchar_t* blanks = L" \t";
    const size_t first = text.find_first_not_of(blanks);
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(blanks);
    return text.substr(first, last - first + 1);
}

// "1/true/yes/on" is true, "0/false/no/off" is false, anything else falls back.
bool ParseBool(const std::string& raw, bool fallback) {
    if (_stricmp(raw.c_str(), "1") == 0 || _stricmp(raw.c_str(), "true") == 0 ||
        _stricmp(raw.c_str(), "yes") == 0 || _stricmp(raw.c_str(), "on") == 0) {
        return true;
    }
    if (_stricmp(raw.c_str(), "0") == 0 || _stricmp(raw.c_str(), "false") == 0 ||
        _stricmp(raw.c_str(), "no") == 0 || _stricmp(raw.c_str(), "off") == 0) {
        return false;
    }
    return fallback;
}

// A [loaders] entry must be a bare file name inside ModuleDir(). Rejecting path
// separators, ".." and absolute paths keeps this key from turning the proxy
// into an arbitrary-DLL loader for anyone who can write version.ini.
bool IsPlainDllName(const std::wstring& name) {
    if (name.empty() || name.size() > kMaxDllNameChars) {
        return false;
    }
    for (const wchar_t c : name) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'"' || c == L'*' ||
            c == L'?' || c == L'<' || c == L'>' || c == L'|') {
            return false;
        }
    }
    if (name.find(L"..") != std::wstring::npos) {
        return false;
    }
    if (name.size() <= 4) {
        return false;
    }
    std::wstring tail = name.substr(name.size() - 4);
    for (wchar_t& c : tail) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return tail == L".dll";
}

bool ListContains(const std::vector<std::wstring>& list, const std::wstring& name) {
    for (const std::wstring& item : list) {
        if (_wcsicmp(item.c_str(), name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// Section value via the Win32 profile API; false when absent or empty.
bool ReadProfileValue(const wchar_t* section, const char* key, std::string& out) {
    const std::filesystem::path& ini = VersionIniPath();
    if (ini.empty()) {
        return false;
    }
    wchar_t wkey[64] = {};
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);

    std::vector<wchar_t> wvalue(kValueChars, L'\0');
    const DWORD n = GetPrivateProfileStringW(section, wkey, L"", wvalue.data(),
                                             kValueChars, ini.c_str());
    if (n == 0 || wvalue[0] == 0) {
        return false;
    }
    char utf8[kUtf8Bytes] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, wvalue.data(), -1, utf8, sizeof utf8,
                            nullptr, nullptr) <= 0) {
        return false;
    }
    out = utf8;
    return true;
}

// Fallback for bare "key=value" files written before the [host] header existed.
// Skips blanks, ';' / '#' comments and section headers; trims quotes and a
// trailing ';' comment. Without this an older deployment would silently read
// nothing and stop attaching.
bool ReadLegacyValue(const char* key, std::string& out) {
    const std::filesystem::path& ini = VersionIniPath();
    if (ini.empty()) {
        return false;
    }
    std::ifstream in(ini, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        const char head = line[first];
        if (head == ';' || head == '#' || head == '[') {
            continue;
        }
        const size_t eq = line.find('=', first);
        if (eq == std::string::npos) {
            continue;
        }
        const std::string name = Trim(line.substr(first, eq - first));
        if (_stricmp(name.c_str(), key) != 0) {
            continue;
        }
        std::string value = line.substr(eq + 1);
        const size_t comment = value.find(';');
        if (comment != std::string::npos) {
            value.erase(comment);
        }
        value = Trim(value);
        if (value.empty()) {
            return false;
        }
        out = value;
        return true;
    }
    return false;
}

bool ReadHostIniValue(const char* key, std::string& out) {
    return ReadProfileValue(kHostSection, key, out) || ReadLegacyValue(key, out);
}

// Seeds the file when it is absent; never overwrites.
bool WriteDefaultIni() {
    const std::filesystem::path& ini = VersionIniPath();
    if (ini.empty()) {
        return false;
    }
    std::ofstream out(ini, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << kDefaultIniText;
    return out.good();
}

// Idempotent: the first caller seeds the file so every reader below agrees on
// what was on disk.
void EnsureIniExists() {
    static const bool done = [] {
        const std::filesystem::path& ini = VersionIniPath();
        if (ini.empty()) {
            return false;
        }
        std::error_code ec;
        if (std::filesystem::exists(ini, ec)) {
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

// Bare (keyless) lines under [loaders], each a plain DLL name. Lets the ini use
//     [loaders]
//     framework.dll
//     framework2.dll
// when the "dlls=" key is not present. Lines containing '=' are skipped so the
// "dlls=a,b" form (handled by LoadDllNames) is never misread as a name.
bool ReadLoaderSectionLines(std::vector<std::wstring>& out) {
    const std::filesystem::path& ini = VersionIniPath();
    if (ini.empty()) {
        return false;
    }
    std::ifstream in(ini, std::ios::binary);
    if (!in) {
        return false;
    }
    bool inLoader = false;
    bool found = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        if (line[first] == '[') {
            const size_t close = line.find(']', first);
            if (close != std::string::npos) {
                const std::string sec =
                    Trim(line.substr(first + 1, close - first - 1));
                inLoader = (_wcsicmp(Widen(sec.c_str()).c_str(), kLoaderSection) == 0);
            }
            continue;
        }
        const char head = line[first];
        if (head == ';' || head == '#' || !inLoader) {
            continue;
        }
        if (line.find('=', first) != std::string::npos) {
            continue;
        }
        const std::wstring name = TrimWide(Widen(line.substr(first).c_str()));
        if (!IsPlainDllName(name) || ListContains(out, name)) {
            continue;
        }
        out.push_back(name);
        found = true;
        if (out.size() >= kMaxLoadDlls) {
            break;
        }
    }
    return found;
}

}  // namespace

const std::filesystem::path& ModuleDir() {
    static const std::filesystem::path dir = SelfDir();
    return dir;
}

const std::filesystem::path& VersionIniPath() {
    static const std::filesystem::path path =
        ModuleDir().empty() ? std::filesystem::path() : ModuleDir() / kIniName;
    return path;
}

bool DebugEnabled() {
    static const bool enabled = [] {
        EnsureIniExists();
        std::string raw;
        if (!ReadHostIniValue(kDebugKey, raw)) {
            return true;   // missing key / empty value: on by default
        }
        return ParseBool(raw, true);
    }();
    return enabled;
}

const wchar_t* TargetExeName() {
    static const std::wstring cached = [] {
        if (VersionIniPath().empty()) {
            return std::wstring();   // cannot resolve our own directory
        }
        EnsureIniExists();
        std::string raw;
        if (ReadHostIniValue(kTargetKey, raw)) {
            const std::wstring wide = Widen(raw.c_str());
            if (!wide.empty()) {
                return wide;
            }
        }
        Log("WARN: %ls has no usable target=, using %s", kIniName, kDefaultTarget);
        return Widen(kDefaultTarget);
    }();
    return cached.c_str();
}

const std::vector<std::wstring>& LoadDllNames() {
    static const std::vector<std::wstring> cached = [] {
        EnsureIniExists();
        std::vector<std::wstring> list;
        ReadLoaderSectionLines(list);   // [loaders] 下逐行写 dll 名
        if (list.empty()) {
            Log("WARN: %ls has no usable [loaders] entries, loading nothing", kIniName);
        }
        return list;
    }();
    return cached;
}