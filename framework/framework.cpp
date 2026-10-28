// ============================================================================
// framework.cpp - MVP 的 framework.dll 实现（组合根）。
//
// 职责（MVP 只验证一条闭环）：
//   浏览器 --WebSocket--> framework.dll --> 宿主窗口（动鼠标/按键/等待）--> 回日志
//
// 设计要点：
//   - 作为独立 DLL，由 version.dll 的 bootstrap 直接 LoadLibrary 后调 NotifyPlatform
//     拿到宿主能力（Host: mainWindow / viewWindow / scale；日志走公共 log 模块）。
//   - 用 CivetWeb（libs/civetweb，C，裸 TCP/WS，零协议风险）起一个
//     127.0.0.1 的 WebSocket 服务端，URI 为 /ws；顺带把 DLL 所在目录当 document_root，
//     可直接 http 托管 index.html。
//   - 事件执行：鼠标/键盘/滚轮/文本走 PostMessage 后台投递到目标窗口（不动物理鼠标）。
//   - 每条命令执行后，把日志写进公共 log 模块（同一份 proxy.log）并通过 WS 回一个 JSON 包。
//
// 模块化拆分（见 framework/ 各 .h/.cpp）：
//   window_target   目标窗口 + 坐标换算 + 虚拟光标
//   input_dispatcher 后台输入派发（PostMessage）
//   script_store    脚本文件持久化（%APPDATA%\TTVoice\scripts）
//   command_handler WS 命令解析与分发 + 几何/光标读数
//   ws_server       CivetWeb 封装（/ws + /api/*）
//   本文件只负责：DLL 生命周期、组装上述模块、对接 picker 广播、端口自测。
//
// 明确不做（见 MVP 规划）：截图取点、OCR、任务调度、热重载、Web 编辑器、JS 脚本层。
// ============================================================================

#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "../common/json.h"
#include "../common/log.h"

#include "civetweb.h"
#include "command_handler.h"
#include "input_dispatcher.h"
#include "host_caps.h"
#include "picker.h"
#include "script_store.h"
#include "ws_server.h"

#include "window_target.h"

namespace {

// 启动返回码：沿用旧 pplugin 的取值，但不再依赖 common/pplugin.h。
constexpr int kStartOk = 0;
constexpr int kStartState = -2;

HMODULE g_hModule = nullptr;
wchar_t g_moduleDir[MAX_PATH] = {};

// picker 模块的广播出口：给某个 WS 连接写一帧文本。
void PickerSend(void* conn, const char* data, size_t len) {
  if (conn != nullptr) {
    mg_websocket_write(static_cast<struct mg_connection*>(conn),
                       MG_WEBSOCKET_OPCODE_TEXT, data, len);
  }
}

const char* ModuleDirUtf8() {
  static char buf[MAX_PATH * 3] = {};
  if (g_moduleDir[0] == L'\0') {
    buf[0] = '.';
    buf[1] = '\0';
    return buf;
  }
  WideCharToMultiByte(CP_UTF8, 0, g_moduleDir, -1, buf, sizeof(buf), nullptr,
                      nullptr);
  return buf;
}

// 在 mg_start 之前自己先绑一次端口，把结果打进 proxy.log（公共 log 模块）。
// 这样即便 CivetWeb 没把 bind 失败原因通过 log_message 回调吐出来，也能定位
// （例如 WSA=10048 表示端口已被占用）。
static void ProbePort(int port) {
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
    LogTagged("framework", "ProbePort: WSAStartup failed");
    return;
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) {
    LogTagged("framework", "ProbePort: socket() failed");
    WSACleanup();
    return;
  }
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u_short>(port));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  int r = bind(s, (sockaddr*)&a, sizeof(a));
  if (r != 0) {
    const int e = WSAGetLastError();
    char buf[192];
    snprintf(buf, sizeof(buf),
             "ProbePort: bind 127.0.0.1:%d failed, WSA=%d", port, e);
    LogTagged("framework", "%s", buf);
  } else {
    char buf[192];
    snprintf(buf, sizeof(buf), "ProbePort: bind 127.0.0.1:%d ok (端口可用)",
             port);
    LogTagged("framework", "%s", buf);
  }
  closesocket(s);
  WSACleanup();
}

// 组合根：持有全部子模块。成员按依赖顺序声明，使引用成员的构造先于使用。
//   WindowTarget / ScriptStore 无依赖；InputDispatcher 依赖 WindowTarget；
//   CommandHandler 依赖 WindowTarget/InputDispatcher；WsServer 依赖
//   CommandHandler/ScriptStore。日志统一走公共 log 模块（组件 framework）。
class Framework {
 public:
  Framework() : input_(win_), handler_(win_, input_) {}

  int start(const HostCaps* host, const char* doc_root, int port) {
    win_.set_host(host);
    ProbePort(port);  // 先自测端口是否可绑定（结果见 proxy.log）
    if (!server_.start(handler_, scripts_, doc_root, port)) {
      LogTagged("framework", "mg_start failed");
      return kStartState;
    }
    if (!PickerStart(host, &PickerSend)) {
      LogTagged("framework", "picker: start failed");
    }
    return kStartOk;
  }

  // 正常卸载（PluginShutdown 调用）：等待推送线程退出。
  void stop() {
    PickerStop();
    server_.stop();
  }

  // 进程卸载（DllMain 调用）：只发退出信号、不等待，避免与加载器锁互等。
  void unload() {
    PickerRequestStop();
    server_.stop();
  }

 private:
  WindowTarget win_;
  InputDispatcher input_;  // 依赖 win_
  ScriptStore scripts_;
  CommandHandler handler_;  // 依赖 win_/input_/scripts_
  WsServer server_;
};

Framework g_fw;

}  // namespace

// ============================================================================
// 新链路：version.dll 直连的 JSON 协议入口（NotifyPlatform）。
//
// version.dll 直接 LoadLibrary(framework.dll) 后调 NotifyPlatform，全部走 JSON；
// 不需要 GetDesc/pplugin。ghost 交来的 dispatch 指针只被包成一张内部 HostCaps
// （三根指针，函数体就是几条 JSON 调用），与插件 ABI 无关。
// ============================================================================
namespace {

typedef int (__cdecl* NotifyGhostFn)(const char*, char*, int, void*);
NotifyGhostFn g_dispatch = nullptr;
void* g_dispatchCtx = nullptr;
bool g_started = false;  // 防止重复 init 把第二次 mg_start 打到已占用的端口

int WriteResp(const char* body, char* resp, int cap) {
  if (body == nullptr || resp == nullptr || cap <= 0) {
    return -1;
  }
  const int need = static_cast<int>(std::strlen(body));
  if (need + 1 > cap) {
    return -(need + 1);
  }
  std::memcpy(resp, body, static_cast<size_t>(need));
  resp[need] = '\0';
  return need;
}

// 调 ghost(request) 并把响应写进 out；返回是否成功。
bool CallBase(const char* req, char* out, int cap) {
  out[0] = '\0';
  if (g_dispatch == nullptr) {
    return false;
  }
  const int n = g_dispatch(req, out, cap, g_dispatchCtx);
  if (n < 0) {
    out[0] = '\0';
    return false;
  }
  return true;
}

// 调 base 取一个 hwnd（"mainWindow"/"viewWindow"），解析 {"hwnd":"..."}。
HWND __cdecl DispatchWindowByCmd(const char* req) {
  char buf[512] = {};
  if (!CallBase(req, buf, sizeof(buf))) {
    return nullptr;
  }
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j;
  char err[32] = {};
  if (!ppjson::Value::Parse(buf, pool, j, err, sizeof(err))) {
    return nullptr;
  }
  const char* hwnd = j.Find("hwnd").AsStr();
  return reinterpret_cast<HWND>(
      static_cast<uintptr_t>(std::strtoull(hwnd, nullptr, 10)));
}

HWND __cdecl DispatchMainWindow() { return DispatchWindowByCmd("{\"cmd\":\"mainWindow\"}"); }
HWND __cdecl DispatchViewWindow() { return DispatchWindowByCmd("{\"cmd\":\"viewWindow\"}"); }

// 让宿主重新解析窗口（应用换主题/开多窗口会重建 HWND，旧句柄就失效了）。
bool __cdecl DispatchRefreshWindow() {
  char buf[256] = {};
  if (!CallBase("{\"cmd\":\"refreshWindow\"}", buf, sizeof(buf))) {
    return false;
  }
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j;
  char err[32] = {};
  if (!ppjson::Value::Parse(buf, pool, j, err, sizeof(err))) {
    return false;
  }
  return j.Find("ok").AsBool(false);
}

float __cdecl DispatchScale() {
  char buf[512] = {};
  if (!CallBase("{\"cmd\":\"scale\"}", buf, sizeof(buf))) {
    return 1.0f;
  }
  ppjson::Node nodes[8] = {};
  ppjson::Pool pool(nodes, 8);
  ppjson::Value j;
  char err[32] = {};
  if (!ppjson::Value::Parse(buf, pool, j, err, sizeof(err))) {
    return 1.0f;
  }
  return static_cast<float>(j.Find("scale").AsNum(1.0));
}

// 用 ghost 的 dispatch 拼出内部能力表，交给现有子模块。
const HostCaps g_hostCaps = {&DispatchMainWindow, &DispatchViewWindow,
                             &DispatchScale, &DispatchRefreshWindow};

int StartViaProtocol() {
  const int port = 47801;
  const char* doc_root = ModuleDirUtf8();
  const int rc = g_fw.start(&g_hostCaps, doc_root, port);
  if (rc == kStartOk) {
    LogTagged("framework", "framework started via protocol (NotifyPlatform)");
  }
  return rc;
}

}  // namespace

extern "C" __declspec(dllexport)
int __cdecl NotifyPlatform(const char* reqJson, char* respJson, int cap) {
  if (reqJson == nullptr || respJson == nullptr || cap <= 0) {
    return -1;
  }
  ppjson::Node nodes[16] = {};
  ppjson::Pool pool(nodes, 16);
  ppjson::Value req;
  char err[64] = {};
  if (!ppjson::Value::Parse(reqJson, pool, req, err, sizeof(err))) {
    return WriteResp("{\"ok\":false,\"err\":\"invalid json\",\"code\":400}", respJson, cap);
  }
  const char* cmd = req.Find("cmd").AsStr();

  if (std::strcmp(cmd, "init") == 0) {
    g_dispatch = reinterpret_cast<NotifyGhostFn>(
        static_cast<uintptr_t>(std::strtoull(req.Find("dispatch").AsStr(), nullptr, 10)));
    g_dispatchCtx = reinterpret_cast<void*>(
        static_cast<uintptr_t>(std::strtoull(req.Find("ctx").AsStr(), nullptr, 10)));
    // 热加载会先 shutdown + FreeLibrary 再 LoadLibrary，正常不会走到这里；
    // 但同一实例被重复 init 时必须幂等，否则第二次 mg_start 会撞端口。
    if (g_started) {
      return WriteResp("{\"ok\":true,\"proto\":1,\"already\":true}", respJson, cap);
    }
    g_started = (StartViaProtocol() == kStartOk);
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "{\"ok\":%s,\"proto\":1}",
                  g_started ? "true" : "false");
    return WriteResp(buf, respJson, cap);
  }
  if (std::strcmp(cmd, "shutdown") == 0) {
    g_fw.stop();
    g_started = false;
    return WriteResp("{\"ok\":true}", respJson, cap);
  }
  char buf[192] = {};
  std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"echo\":\"%s\"}", cmd);
  return WriteResp(buf, respJson, cap);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hModule = hModule;
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(hModule, buf, MAX_PATH) > 0) {
      // 去掉文件名，保留目录：找最后一个反斜杠并截断（无 STL）。
      wchar_t* last = wcsrchr(buf, L'\\');
      if (last != nullptr) {
        *last = L'\0';
      }
      wcscpy_s(g_moduleDir, MAX_PATH, buf);
    }
    DisableThreadLibraryCalls(hModule);
  } else if (reason == DLL_PROCESS_DETACH) {
    g_fw.unload();
  }
  return TRUE;
}
