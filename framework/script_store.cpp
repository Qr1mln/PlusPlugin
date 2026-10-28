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

void WriteFail(ppjson::Pool& pool, ppjson::Value& out, const char* data) {
  out = ppjson::Value::Obj(pool);
  out.SetBool("success", false);
  out.SetStr("data", (data != nullptr) ? data : "");
  out.SetStr("code", kErr);
}

void WriteOk(ppjson::Pool& pool, ppjson::Value& out, ppjson::Value data) {
  out = ppjson::Value::Obj(pool);
  out.SetBool("success", true);
  if (data.Valid()) {
    out.SetValue("data", data);
  } else {
    out.SetValue("data", ppjson::Value::Str(pool, ""));
  }
  out.SetStr("code", kOk);
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

void ScriptStore::list(ppjson::Pool& pool, ppjson::Value& out) const {
  ppjson::Value arr = ppjson::Value::Arr(pool);
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
    arr.AddStr(s.c_str());
  }
  WriteOk(pool, out, arr);
}

void ScriptStore::read(ppjson::Pool& pool, const std::string& name,
                       ppjson::Value& out) const {
  const std::string text = read_text_utf8(script_path(name));
  if (text.empty()) {
    WriteFail(pool, out, "script not found");
    return;
  }
  ppjson::Value parsed;
  char err[64] = {};
  if (!ppjson::Value::Parse(text.c_str(), pool, parsed, err, sizeof(err))) {
    WriteFail(pool, out, "bad json");
    return;
  }
  WriteOk(pool, out, parsed);
}

void ScriptStore::write(ppjson::Pool& pool, const std::string& name,
                        const std::string& payload, ppjson::Value& out) {
  ppjson::Value parsed;
  char err[64] = {};
  if (!ppjson::Value::Parse(payload.c_str(), pool, parsed, err, sizeof(err))) {
    WriteFail(pool, out, "bad json");
    return;
  }
  if (!parsed.IsObj()) {
    WriteFail(pool, out, "not an object");
    return;
  }

  char body[65536] = {};
  const int n = parsed.Dump(body, sizeof(body));
  if (n < 0) {
    WriteFail(pool, out, "script too large");
    return;
  }

  const std::filesystem::path path = script_path(name);
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  {
    std::ofstream fout(tmp, std::ios::binary | std::ios::trunc);
    if (!fout) {
      WriteFail(pool, out, "open failed");
      return;
    }
    fout.write(body, n);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    WriteFail(pool, out, "write failed");
    return;
  }
  WriteOk(pool, out, ppjson::Value());
}

void ScriptStore::remove(ppjson::Pool& pool, const std::string& name,
                         ppjson::Value& out) {
  std::error_code ec;
  const bool removed = std::filesystem::remove(script_path(name), ec);
  if (removed) {
    WriteOk(pool, out, ppjson::Value());
    return;
  }
  WriteFail(pool, out, "script not found");
}