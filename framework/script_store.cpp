// ============================================================================
// script_store.cpp - 见 script_store.h。
// ============================================================================
#include "script_store.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <iterator>
#include <vector>

#include <windows.h>

namespace {

const char* kOk = "0000";
const char* kErr = "9999";

nlohmann::json Fail(const std::string& data) {
  nlohmann::json r;
  r["success"] = false;
  r["data"] = data;
  r["code"] = kErr;
  return r;
}

nlohmann::json Ok(const nlohmann::json& data) {
  nlohmann::json r;
  r["success"] = true;
  r["data"] = data;
  r["code"] = kOk;
  return r;
}

}  // namespace

bool ScriptStore::safe_name(const std::string& name) {
  if (name.empty() || name.find("..") != std::string::npos) {
    return false;
  }
  for (const char c : name) {
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
  }
  return true;
}

std::filesystem::path ScriptStore::scripts_dir() {
  wchar_t appdata[32768] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
  if (n == 0 || n >= 32768) {
    return std::filesystem::path();
  }
  return std::filesystem::path(appdata) / L"TTVoice" / L"scripts";
}

std::filesystem::path ScriptStore::script_path(const std::string& utf8_name) {
  std::wstring wide;
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(), -1, nullptr, 0);
  if (n > 0) {
    wide.resize(static_cast<size_t>(n - 1));
    MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(), -1, &wide[0], n);
  }
  return scripts_dir() / (wide + L".json");
}

std::string ScriptStore::read_text_utf8(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::string();
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

nlohmann::json ScriptStore::list() const {
  nlohmann::json arr = nlohmann::json::array();
  std::error_code ec;
  const std::filesystem::path dir = scripts_dir();
  std::vector<std::string> names;
  if (!dir.empty() && std::filesystem::is_directory(dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file()) {
        continue;
      }
      if (_wcsicmp(entry.path().extension().c_str(), L".json") != 0) {
        continue;
      }
      names.push_back(entry.path().stem().u8string());
    }
  }
  std::sort(names.begin(), names.end());
  for (const std::string& s : names) {
    arr.push_back(s);
  }
  return Ok(arr);
}

nlohmann::json ScriptStore::read(const std::string& name) const {
  const std::string text = read_text_utf8(script_path(name));
  if (text.empty()) {
    return Fail("script not found");
  }
  try {
    return Ok(nlohmann::json::parse(text));
  } catch (...) {
    return Fail("bad json");
  }
}

nlohmann::json ScriptStore::write(const std::string& name,
                                  const std::string& payload) {
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(payload);
  } catch (...) {
    return Fail("bad json");
  }
  if (!parsed.is_object()) {
    return Fail("not an object");
  }
  const std::filesystem::path path = script_path(name);
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Fail("open failed");
    }
    out << parsed.dump(2);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return Fail("write failed");
  }
  return Ok("");
}

nlohmann::json ScriptStore::remove(const std::string& name) {
  std::error_code ec;
  const bool removed = std::filesystem::remove(script_path(name), ec);
  if (removed) {
    return Ok("");
  }
  return Fail("script not found");
}
