// ============================================================================
// script_store.h - 脚本文件的持久化（%APPDATA%\TTVoice\scripts\<name>.json）。
//
// 只负责文件系统操作，不碰 HTTP；每个方法把统一封套 {success,data,code}
// 写进调用方的 out（由上层 WsServer 提供 pool 并发回）。
// 无 STL：参数用 const char*，路径走 Win32（wchar_t 缓冲）。
// ============================================================================
#ifndef FRAMEWORK_SCRIPT_STORE_H
#define FRAMEWORK_SCRIPT_STORE_H

#include "../common/json_util.h"

class ScriptStore {
 public:
  // 脚本名只允许普通文件名，禁止路径穿越（".." / \ / :）。
  static bool safe_name(const char* name);

  void list(ppjson::Pool& pool, ppjson::Value& out) const;
  void read(ppjson::Pool& pool, const char* name, ppjson::Value& out) const;
  void write(ppjson::Pool& pool, const char* name, const char* payload,
             ppjson::Value& out);
  void remove(ppjson::Pool& pool, const char* name, ppjson::Value& out);

 private:
  // 拼出 <scriptsDir>\<name>.json 到 out（wchar）。返回 false 表示无法解析。
  static bool build_path(const char* name, wchar_t* out, int cap);
  // 读整个 UTF-8 文本文件，返回字节数（-1 表示失败）。
  static int read_text(const wchar_t* path, char* out, int cap);
  // 目录：%APPDATA%\TTVoice\scripts。
  static bool scripts_dir(wchar_t* out, int cap);
};

#endif  // FRAMEWORK_SCRIPT_STORE_H