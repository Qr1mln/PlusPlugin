// common/log.cpp - 见 log.h。
//
// 跨 DLL 串行化：version.dll 与 framework.dll 各编译一份，但都用同名命名互斥量
// （CreateMutexW 同名 -> 同一内核对象），从而同一进程内写同一文件不会交错。
#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr int kLogLineChars = 1024;
constexpr int kLogPathChars = 512;

// 同名互斥量：两个 DLL 在进程内得到同一内核对象，串行化文件写。
const wchar_t* kLogMutexName = L"Local\\PlusPluginLogMutex_v1";

// 进程内唯一的日志互斥量句柄（缓存，进程生命周期内不关闭）。
HANDLE LogMutex() {
  static HANDLE m = CreateMutexW(nullptr, FALSE, kLogMutexName);
  return m;
}

// 计算「本模块所在目录 \ proxy.log」。用 FROM_ADDRESS 取包含本编译单元模块自身的
// 句柄，这样无论编进 version.dll 还是 framework.dll，都能定位到 %APPDATA%\TTVoice。
const wchar_t* LogPath() {
  static wchar_t path[kLogPathChars] = {};
  static const bool init = [] {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LogPath), &self) ||
        self == nullptr) {
      self = GetModuleHandleW(nullptr);  // 退回 EXE 路径
    }
    wchar_t dir[kLogPathChars] = {};
    const DWORD n = GetModuleFileNameW(self, dir, kLogPathChars);
    if (n == 0 || n >= static_cast<DWORD>(kLogPathChars)) {
      wcscpy_s(path, kLogPathChars, L"proxy.log");
      return true;
    }
    wchar_t* last = wcsrchr(dir, L'\\');
    if (last != nullptr) {
      *last = L'\0';
    }
    swprintf_s(path, kLogPathChars, L"%s\\proxy.log", dir);
    return true;
  }();
  (void)init;
  return path;
}

void LogV(const char* component, const char* fmt, va_list ap) {
  char line[kLogLineChars] = {};
  vsnprintf(line, sizeof line, fmt, ap);

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char stamp[128] = {};
  snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u]-[%s] ",
           static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
           static_cast<unsigned>(st.wSecond),
           static_cast<unsigned>(st.wMilliseconds),
           (component != nullptr && component[0] != 0) ? component : "proxy");

  // 一条记录一次写，锁内完成：不撕裂、不交错。
  char record[kLogLineChars + 128] = {};
  snprintf(record, sizeof record, "%s%s\n", stamp, line);
  const int recordLen = static_cast<int>(std::strlen(record));

  // 源字面量是 UTF-8，但 Windows 记事本/控制台默认按 ANSI(GBK) 解读 -> 中文乱码。
  // 这里把整条记录从 UTF-8 转成本地 ANSI 再写，保证中文可读；转换失败则退回原字节。
  // 注意：wbuf/abuf 必须活到下方 WriteFile 之后，不能放进会离开作用域的块（否则悬空指针）。
  wchar_t wbuf[kLogLineChars + 128] = {};
  char abuf[2 * (kLogLineChars + 128)] = {};
  const char* outBuf = record;
  int outLen = recordLen;
  const int wn = MultiByteToWideChar(CP_UTF8, 0, record, recordLen, wbuf,
                                     kLogLineChars + 128);
  if (wn > 0) {
    const int an = WideCharToMultiByte(CP_ACP, 0, wbuf, wn, abuf, sizeof abuf,
                                       nullptr, nullptr);
    if (an > 0) {
      outBuf = abuf;
      outLen = an;
    }
  }

  HANDLE m = LogMutex();
  if (m != nullptr) {
    WaitForSingleObject(m, INFINITE);
  }
  HANDLE h = CreateFileW(LogPath(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(h, outBuf, static_cast<DWORD>(outLen), &written, nullptr);
    CloseHandle(h);
  }
  if (m != nullptr) {
    ReleaseMutex(m);
  }

  // 镜像给插件订阅者（新链路通常为 null，且 fan-out 自带防递归）。
  if (g_logFanout != nullptr) {
    g_logFanout(line);
  }
}

}  // namespace

void (*g_logFanout)(const char* line) = nullptr;

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogV("proxy", fmt, ap);
  va_end(ap);
}

void LogTagged(const char* component, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogV(component, fmt, ap);
  va_end(ap);
}
