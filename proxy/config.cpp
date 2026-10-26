#include "config.h"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <string>

#include "log.h"

namespace {

constexpr wchar_t kTargetIniName[] = L"target.ini";
constexpr wchar_t kHostSection[] = L"host";
constexpr char kTargetKey[] = "target";
constexpr char kDebugKey[] = "debug";
constexpr char kDefaultTarget[] = "test.exe";

// target.ini is ASCII on purpose: the Win32 profile API reads it as ANSI, so
// keep keys and values ASCII and stick to ';' comments.
constexpr char kDefaultIniText[] =
    "; host process for version.dll, e.g. target=MyApp.exe\r\n"
    "[host]\r\n"
    "target=test.exe\r\n"
    "; pop a dialog when the proxy is attached (default true)\r\n"
    "debug=true\r\n";

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

// [host] value via the Win32 profile API; false when absent or empty.
bool ReadProfileValue(const char* key, std::string& out) {
    const std::filesystem::path& ini = TargetIniPath();
    if (ini.empty()) {
        return false;
    }
    wchar_t wkey[64] = {};
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);

    wchar_t wvalue[512] = {};
    const DWORD n = GetPrivateProfileStringW(kHostSection, wkey, L"", wvalue, 512, ini.c_str());
    if (n == 0 || wvalue[0] == 0) {
        return false;
    }
    char utf8[1024] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, utf8, sizeof utf8, nullptr, nullptr) <= 0) {
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
    const std::filesystem::path& ini = TargetIniPath();
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

bool ReadIniValue(const char* key, std::string& out) {
    return ReadProfileValue(key, out) || ReadLegacyValue(key, out);
}

// Seeds [host] target/debug when the file is absent; never overwrites.
bool WriteDefaultTargetIni() {
    const std::filesystem::path& ini = TargetIniPath();
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

}  // namespace

const std::filesystem::path& ModuleDir() {
    static const std::filesystem::path dir = SelfDir();
    return dir;
}

const std::filesystem::path& TargetIniPath() {
    static const std::filesystem::path path =
        ModuleDir().empty() ? std::filesystem::path() : ModuleDir() / kTargetIniName;
    return path;
}

bool DebugEnabled() {
    static const bool enabled = [] {
        std::string raw;
        if (!ReadIniValue(kDebugKey, raw)) {
            return true;   // missing key / empty value: on by default
        }
        return ParseBool(raw, true);
    }();
    return enabled;
}

const wchar_t* TargetExeName() {
    static const std::wstring cached = [] {
        std::string raw;
        if (ReadIniValue(kTargetKey, raw)) {
            const std::wstring wide = Widen(raw.c_str());
            if (!wide.empty()) {
                return wide;
            }
        }

        const std::filesystem::path& ini = TargetIniPath();
        if (ini.empty()) {
            return std::wstring();
        }
        // No usable target=: seed the file when missing, then use the
        // placeholder so both sides agree on what was read.
        std::error_code ec;
        if (!std::filesystem::exists(ini, ec)) {
            if (WriteDefaultTargetIni()) {
                Log("%ls not found, created with target=%s", kTargetIniName, kDefaultTarget);
            } else {
                Log("WARN: %ls not found and could not be created", kTargetIniName);
            }
        } else {
            Log("WARN: %ls has no usable target=, using %s", kTargetIniName, kDefaultTarget);
        }
        return Widen(kDefaultTarget);
    }();
    return cached.c_str();
}
