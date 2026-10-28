// ============================================================================
// script_store.cpp - 见 script_store.h。
//
// 无 STL：文件系统走 Win32（GetEnvironmentVariableW / FindFirstFileW /
// CreateFileW / ReadFile / WriteFile / DeleteFileW / MoveFileExW）。
// ============================================================================
#include "script_store.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace {

const char* kOk = "0000";
const char* kErr = "9999";

const int kPathCap = 512;    // 目录/路径 wchar 缓冲
const int kMaxScripts = 128; // 列出的脚本名上限
const int kNameCap = 128;    // 单个脚本名（wchar）

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

// UTF-8 -> wide（NUL 结尾），返回是否成功。
bool Utf8ToWide(const char* s, wchar_t* out, int cap) {
  if (out == nullptr || cap <= 0) {
    return false;
  }
  out[0] = L'\0';
  if (s == nullptr || s[0] == '\0') {
    return false;
  }
  return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

// 宽字符串末尾（不含扩展名）追加 ".json"：这里直接由 name 拼，见 build_path。
}  // namespace

bool ScriptStore::safe_name(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  if (std::strstr(name, "..") != nullptr) {
    return false;
  }
  for (const char* p = name; *p != '\0'; ++p) {
    const char c = *p;
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
  }
  return true;
}

bool ScriptStore::scripts_dir(wchar_t* out, int cap) {
  if (out == nullptr || cap <= 0) {
    return false;
  }
  wchar_t appdata[kPathCap] = {};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, kPathCap);
  if (n == 0 || n >= static_cast<DWORD>(kPathCap)) {
    return false;
  }
  return swprintf_s(out, static_cast<size_t>(cap), L"%s\\TTVoice\\scripts",
                    appdata) > 0;
}

bool ScriptStore::build_path(const char* name, wchar_t* out, int cap) {
  wchar_t dir[kPathCap] = {};
  if (!scripts_dir(dir, kPathCap)) {
    return false;
  }
  wchar_t wide[kNameCap] = {};
  if (!Utf8ToWide(name, wide, kNameCap)) {
    return false;
  }
  return swprintf_s(out, static_cast<size_t>(cap), L"%s\\%s.json", dir, wide) > 0;
}

int ScriptStore::read_text(const wchar_t* path, char* out, int cap) {
  if (out == nullptr || cap <= 0) {
    return -1;
  }
  out[0] = '\0';
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return -1;
  }
  DWORD got = 0;
  const BOOL ok = ReadFile(h, out, static_cast<DWORD>(cap - 1), &got, nullptr);
  CloseHandle(h);
  if (!ok) {
    return -1;
  }
  if (static_cast<int>(got) >= cap) {
    got = static_cast<DWORD>(cap - 1);
  }
  out[got] = '\0';
  return static_cast<int>(got);
}

void ScriptStore::list(ppjson::Pool& pool, ppjson::Value& out) const {
  ppjson::Value arr = ppjson::Value::Arr(pool);
  wchar_t dir[kPathCap] = {};
  if (scripts_dir(dir, kPathCap)) {
    wchar_t pattern[kPathCap] = {};
    swprintf_s(pattern, L"%s\\*.json", dir);
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      // 收集文件名（去扩展名），必要时插入排序。
      wchar_t names[kMaxScripts][kNameCap] = {};
      int count = 0;
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          continue;
        }
        wcscpy_s(names[count], kNameCap, fd.cFileName);
        // 去掉 ".json"
        const int len = static_cast<int>(wcslen(names[count]));
        if (len > 5 && _wcsicmp(names[count] + len - 5, L".json") == 0) {
          names[count][len - 5] = L'\0';
        }
        ++count;
        if (count >= kMaxScripts) {
          break;
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);

      // 简单插入排序（按宽字符序）。
      for (int i = 1; i < count; ++i) {
        wchar_t key[kNameCap] = {};
        wcscpy_s(key, kNameCap, names[i]);
        int j = i - 1;
        while (j >= 0 && wcscmp(names[j], key) > 0) {
          wcscpy_s(names[j + 1], kNameCap, names[j]);
          --j;
        }
        wcscpy_s(names[j + 1], kNameCap, key);
      }

      for (int i = 0; i < count; ++i) {
        char utf8[kNameCap * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, names[i], -1, utf8, sizeof(utf8), nullptr, nullptr);
        arr.AddStr(utf8);
      }
    }
  }
  WriteOk(pool, out, arr);
}

void ScriptStore::read(ppjson::Pool& pool, const char* name,
                       ppjson::Value& out) const {
  wchar_t path[kPathCap] = {};
  if (!build_path(name, path, kPathCap)) {
    WriteFail(pool, out, "script not found");
    return;
  }
  char text[65536] = {};
  if (read_text(path, text, sizeof(text)) < 0 || text[0] == '\0') {
    WriteFail(pool, out, "script not found");
    return;
  }
  ppjson::Value parsed;
  char err[64] = {};
  if (!ppjson::Value::Parse(text, pool, parsed, err, sizeof(err))) {
    WriteFail(pool, out, "bad json");
    return;
  }
  WriteOk(pool, out, parsed);
}

void ScriptStore::write(ppjson::Pool& pool, const char* name,
                        const char* payload, ppjson::Value& out) {
  ppjson::Value parsed;
  char err[64] = {};
  const char* src = (payload != nullptr) ? payload : "";
  if (!ppjson::Value::Parse(src, pool, parsed, err, sizeof(err))) {
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

  wchar_t path[kPathCap] = {};
  if (!build_path(name, path, kPathCap)) {
    WriteFail(pool, out, "bad name");
    return;
  }
  wchar_t tmp[kPathCap] = {};
  swprintf_s(tmp, L"%s.tmp", path);

  HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    WriteFail(pool, out, "open failed");
    return;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, body, static_cast<DWORD>(n), &written, nullptr);
  CloseHandle(h);
  if (!ok || static_cast<int>(written) != n) {
    DeleteFileW(tmp);
    WriteFail(pool, out, "write failed");
    return;
  }
  // 原子替换：MoveFileEx 覆盖目标（失败则退回先删再移）。
  if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(path);
    if (!MoveFileExW(tmp, path, 0)) {
      DeleteFileW(tmp);
      WriteFail(pool, out, "write failed");
      return;
    }
  }
  WriteOk(pool, out, ppjson::Value());
}

void ScriptStore::remove(ppjson::Pool& pool, const char* name,
                         ppjson::Value& out) {
  wchar_t path[kPathCap] = {};
  if (!build_path(name, path, kPathCap)) {
    WriteFail(pool, out, "script not found");
    return;
  }
  if (DeleteFileW(path)) {
    WriteOk(pool, out, ppjson::Value());
    return;
  }
  WriteFail(pool, out, "script not found");
}