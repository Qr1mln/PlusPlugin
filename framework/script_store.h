// ============================================================================
// script_store.h - 脚本文件的持久化（%APPDATA%\TTVoice\scripts\<name>.json）。
//
// 只负责文件系统操作，不碰 HTTP；每个方法把统一封套 {success,data,code}
// 写进调用方的 out（由上层 WsServer 的 /api/scripts 处理器提供 pool 并发回）。
// 路径穿越由 safe_name 拦截。JSON 用共享的 common/json_util。
// ============================================================================
#ifndef FRAMEWORK_SCRIPT_STORE_H
#define FRAMEWORK_SCRIPT_STORE_H

#include <filesystem>
#include <string>

#include "../common/json_util.h"

class ScriptStore {
 public:
  // 脚本名只允许普通文件名，禁止路径穿越（".." / \ / :）。
  static bool safe_name(const std::string& name);

  // 列出全部脚本名（按字母序）。对 out 写 {success:true,data:[..],code:"0000"}。
  void list(ppjson::Pool& pool, ppjson::Value& out) const;
  // 读取单个脚本；不存在写 {success:false,data:"script not found"}。
  void read(ppjson::Pool& pool, const std::string& name, ppjson::Value& out) const;
  // 新建/保存：解析 payload 为对象后原子写入（写 .tmp 再 rename）。
  void write(ppjson::Pool& pool, const std::string& name,
             const std::string& payload, ppjson::Value& out);
  // 删除。成功 {success:true}，不存在 {success:false}（语义与原文一致）。
  void remove(ppjson::Pool& pool, const std::string& name, ppjson::Value& out);

 private:
  static std::filesystem::path scripts_dir();
  static std::filesystem::path script_path(const std::string& utf8_name);
  static std::string read_text_utf8(const std::filesystem::path& path);
};

#endif  // FRAMEWORK_SCRIPT_STORE_H