// ============================================================================
// logger.cpp - 见 logger.h。
// ============================================================================
#include "logger.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "pplugin.h"

void Logger::publish(const char* fmt, ...) noexcept {
  if (host_ == nullptr || host_->publishLog == nullptr || fmt == nullptr) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = 0;
  host_->publishLog(buf);
}

void Logger::publish_line(const char* line) noexcept {
  if (host_ != nullptr && host_->publishLog != nullptr && line != nullptr) {
    host_->publishLog(line);
  }
}

void Logger::publish_line_prefix(const char* prefix, const char* line) noexcept {
  if (host_ == nullptr || host_->publishLog == nullptr) {
    return;
  }
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "%s%s", (prefix != nullptr) ? prefix : "",
                (line != nullptr) ? line : "");
  buf[sizeof(buf) - 1] = 0;
  host_->publishLog(buf);
}
