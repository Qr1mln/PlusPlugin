// ============================================================================
// picker.cpp - 取点模块实现（协议见 picker.h）。
//
// 两条数据流：
//   1) 光标流：有订阅者时，每 33ms 采样一次 GetCursorPos + ScreenToClient 后广播；
//   2) 拾取：装 WH_MOUSE_LL 钩子，落在 view 内的左键被吞掉并广播坐标，右键取消。
//
// 关键点：
//   - 低级鼠标钩子由安装它的线程派发，所以本模块的推送线程保持消息循环即可，
//     钩子回调天然与采样同线程，不需要额外同步采样状态。
//   - 广播时持有 g_mutex；连接 close 回调也拿这把锁摘连接，避免写到已回收的 conn。
// ============================================================================

#include "picker.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

const Host* g_host = nullptr;
PickerBroadcast g_broadcast = nullptr;

// 推送线程的消息 id
const UINT kMsgPickStart = WM_APP + 1;  // 装钩子，进入拾取态
const UINT kMsgPickFlush = WM_APP + 2;  // 发掉待发拾取结果（wParam=1 时发完卸钩子）
const UINT_PTR kTimerId = 1;
const UINT kTickMs = 33;  // 约 30Hz

// WS 连接表：conn 由 framework.cpp 传入（mg_connection*），watch=是否订阅光标流
struct Client {
    void* conn;
    bool watch;
};
std::mutex g_mutex;  // 保护 g_clients / watch 状态 / 待发拾取结果
std::vector<Client> g_clients;
std::atomic<int> g_watchCount(0);
std::atomic<bool> g_pickActive(false);

std::string g_pendingPick;
bool g_pendingReady = false;

// 钩子状态（只由推送线程访问）
HHOOK g_hook = nullptr;
bool g_swallowLeftUp = false;
bool g_swallowRightUp = false;

HANDLE g_thread = nullptr;
volatile DWORD g_tid = 0;

void LogLine(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    if (g_host != nullptr && g_host->publishLog != nullptr) {
        g_host->publishLog(buf);
    }
}

// 目标窗口：优先 viewWindow（真正收输入的 Flutter 子窗口），回退 mainWindow。
HWND TargetWindow() {
    HWND h = nullptr;
    if (g_host != nullptr && g_host->viewWindow != nullptr) {
        h = g_host->viewWindow();
    }
    if (h == nullptr && g_host != nullptr && g_host->mainWindow != nullptr) {
        h = g_host->mainWindow();
    }
    return h;
}

double HostScale() {
    const double s = (g_host != nullptr && g_host->scale != nullptr) ? g_host->scale() : 1.0;
    return (s > 0.0) ? s : 1.0;
}

double Round1(double v) {
    return (v >= 0.0) ? std::floor(v * 10.0 + 0.5) / 10.0 : std::ceil(v * 10.0 - 0.5) / 10.0;
}

// 把一帧文本发给全部（或仅订阅的）连接。
// 写在持锁区间内，保证用到的 conn 不会被并发回收；wait=false 时拿不到锁就跳过这一帧
// （连接正在关闭），避免与 CivetWeb 的关闭路径互相等待。
void Broadcast(const std::string& msg, bool onlyWatch, bool wait) {
    if (g_broadcast == nullptr || msg.empty()) {
        return;
    }
    std::unique_lock<std::mutex> lk(g_mutex, std::defer_lock);
    if (wait) {
        lk.lock();
    } else if (!lk.try_lock()) {
        return;
    }
    for (const Client& c : g_clients) {
        if (onlyWatch && !c.watch) {
            continue;
        }
        g_broadcast(c.conn, msg.c_str(), msg.size());
    }
}

// 排一条拾取结果并唤醒推送线程立刻发出（stopAfter=true 时发完卸钩子）。
void QueuePick(const char* json, bool stopAfter) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingPick = (json != nullptr) ? json : "";
        g_pendingReady = true;
    }
    const DWORD tid = g_tid;
    if (tid != 0) {
        PostThreadMessageW(tid, kMsgPickFlush, stopAfter ? 1 : 0, 0);
    }
}

// 低级鼠标钩子：拾取态下吞掉 view 内的左键并上报坐标；右键取消（同样吞掉）。
LRESULT CALLBACK MouseHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || !g_pickActive.load()) {
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }
    if (wParam == WM_LBUTTONDOWN) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const HWND wnd = TargetWindow();
        if (ms != nullptr && wnd != nullptr) {
            POINT cp = ms->pt;
            RECT rc;
            if (ScreenToClient(wnd, &cp) && GetClientRect(wnd, &rc) && cp.x >= 0 && cp.y >= 0 &&
                cp.x < rc.right && cp.y < rc.bottom) {
                const double scale = HostScale();
                ppjson::Node nodes[16] = {};
                ppjson::Pool pool(nodes, 16);
                ppjson::Value p = ppjson::Value::Obj(pool);
                p.SetStr("t", "pick");
                p.SetNum("sx", ms->pt.x);
                p.SetNum("sy", ms->pt.y);
                p.SetNum("x", cp.x);
                p.SetNum("y", cp.y);
                p.SetNum("lx", Round1(cp.x / scale));
                p.SetNum("ly", Round1(cp.y / scale));
                p.SetNum("scale", scale);
                char jbuf[256] = {};
                p.Dump(jbuf, sizeof(jbuf));
                g_swallowLeftUp = true;
                QueuePick(jbuf, true);
                return 1;  // 吞掉这次点击，应用不响应
            }
        }
    } else if (wParam == WM_LBUTTONUP && g_swallowLeftUp) {
        g_swallowLeftUp = false;
        return 1;
    } else if (wParam == WM_RBUTTONDOWN) {
        g_swallowRightUp = true;
        ppjson::Node nodes[8] = {};
        ppjson::Pool pool(nodes, 8);
        ppjson::Value p = ppjson::Value::Obj(pool);
        p.SetStr("t", "pick");
        p.SetStr("action", "cancel");
        char jbuf[128] = {};
        p.Dump(jbuf, sizeof(jbuf));
        QueuePick(jbuf, true);
        return 1;
    } else if (wParam == WM_RBUTTONUP && g_swallowRightUp) {
        g_swallowRightUp = false;
        return 1;
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

void PickStopInThread() {
    if (g_hook != nullptr) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        LogLine("pick: hook removed");
    }
    g_swallowLeftUp = false;
    g_swallowRightUp = false;
    g_pickActive.store(false);
}

void PickStartInThread() {
    if (g_hook != nullptr) {
        return;
    }
    g_swallowLeftUp = false;
    g_swallowRightUp = false;
    // 低级钩子不需要跨进程注入，hMod 传 NULL 即可。
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookProc, nullptr, 0);

    ppjson::Node nodes[8] = {};
    ppjson::Pool pool(nodes, 8);
    ppjson::Value p = ppjson::Value::Obj(pool);
    p.SetStr("t", "pick");
    if (g_hook == nullptr) {
        g_pickActive.store(false);
        const DWORD err = GetLastError();
        LogLine("pick: SetWindowsHookEx failed err=%lu", err);
        p.SetStr("action", "failed");
        p.SetStr("error", "set hook failed");
    } else {
        g_pickActive.store(true);
        LogLine("pick: hook installed (左键拾取 / 右键取消)");
        p.SetStr("action", "ready");
    }
    char jbuf[256] = {};
    p.Dump(jbuf, sizeof(jbuf));
    Broadcast(jbuf, false, true);
}

void PickFlushInThread(bool stopAfter) {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_pendingReady) {
            msg.swap(g_pendingPick);
            g_pendingReady = false;
        }
    }
    if (!msg.empty()) {
        Broadcast(msg, false, true);
    }
    if (stopAfter) {
        PickStopInThread();
    }
}

void PushCursorTick() {
    if (g_watchCount.load() <= 0) {
        return;
    }
    ppjson::Node nodes[16] = {};
    ppjson::Pool pool(nodes, 16);
    ppjson::Value c = ppjson::Value::Obj(pool);
    if (!PickerCursorInfo(c)) {
        return;
    }
    c.SetStr("t", "cursor");
    char jbuf[384] = {};
    c.Dump(jbuf, sizeof(jbuf));
    Broadcast(jbuf, true, false);
}

DWORD WINAPI PushThreadProc(LPVOID) {
    g_tid = GetCurrentThreadId();
    SetTimer(nullptr, kTimerId, kTickMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) {
            PushCursorTick();
        } else if (msg.message == kMsgPickStart) {
            PickStartInThread();
        } else if (msg.message == kMsgPickFlush) {
            PickFlushInThread(msg.wParam != 0);
        } else {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    PickStopInThread();
    KillTimer(nullptr, kTimerId);
    g_tid = 0;
    return 0;
}

}  // namespace

bool PickerStart(const Host* host, PickerBroadcast broadcast) {
    if (host == nullptr || broadcast == nullptr) {
        return false;
    }
    g_host = host;
    g_broadcast = broadcast;
    if (g_thread != nullptr) {
        return true;
    }
    g_thread = CreateThread(nullptr, 0, &PushThreadProc, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        LogLine("picker: CreateThread failed err=%lu", GetLastError());
        return false;
    }
    LogLine("picker: push thread started");
    return true;
}

void PickerRequestStop() {
    const DWORD tid = g_tid;
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    // 不等待线程退出：DllMain 里 join 会与加载器锁互等。
}

void PickerStop() {
    if (g_thread == nullptr) {
        return;
    }
    PickerRequestStop();
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_tid = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_clients.clear();
        g_watchCount.store(0);
    }
    LogLine("picker: push thread stopped");
}

void PickerAddClient(void* conn) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_clients.push_back(Client{conn, false});
}

void PickerRemoveClient(void* conn) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (size_t i = 0; i < g_clients.size(); ++i) {
        if (g_clients[i].conn == conn) {
            if (g_clients[i].watch) {
                g_watchCount.fetch_sub(1);
            }
            g_clients.erase(g_clients.begin() + static_cast<long>(i));
            break;
        }
    }
}

bool PickerSetWatch(void* conn, bool on) {
    if (conn == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    for (Client& c : g_clients) {
        if (c.conn == conn) {
            if (c.watch != on) {
                c.watch = on;
                if (on) {
                    g_watchCount.fetch_add(1);
                } else {
                    g_watchCount.fetch_sub(1);
                }
            }
            return on;
        }
    }
    return false;
}

bool PickerBeginPick() {
    const DWORD tid = g_tid;
    if (tid == 0) {
        return false;
    }
    PostThreadMessageW(tid, kMsgPickStart, 0, 0);
    return true;
}

void PickerCancelPick() {
    ppjson::Node nodes[8] = {};
    ppjson::Pool pool(nodes, 8);
    ppjson::Value p = ppjson::Value::Obj(pool);
    p.SetStr("t", "pick");
    p.SetStr("action", "cancel");
    char jbuf[128] = {};
    p.Dump(jbuf, sizeof(jbuf));
    QueuePick(jbuf, true);
}

// 一次性光标读数：屏幕坐标、view 客户区物理像素、逻辑像素（x/scale）、窗口尺寸。
bool PickerCursorInfo(ppjson::Value& out) {
    const HWND wnd = TargetWindow();
    if (wnd == nullptr) {
        return false;
    }
    POINT sp = {0, 0};
    if (!GetCursorPos(&sp)) {
        return false;
    }
    RECT rc;
    if (!GetClientRect(wnd, &rc)) {
        return false;
    }
    POINT cp = sp;
    if (!ScreenToClient(wnd, &cp)) {
        return false;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const double scale = HostScale();
    out.SetNum("sx", sp.x);
    out.SetNum("sy", sp.y);
    out.SetNum("x", cp.x);
    out.SetNum("y", cp.y);
    out.SetNum("lx", Round1(cp.x / scale));
    out.SetNum("ly", Round1(cp.y / scale));
    out.SetNum("w", w);
    out.SetNum("h", h);
    out.SetNum("scale", scale);
    out.SetBool("inside", (cp.x >= 0 && cp.y >= 0 && cp.x < w && cp.y < h));
    // 页面此时没有焦点，浏览器收不到按键，只能由宿主进程采样全局键态。
    out.SetBool("f8", ((GetAsyncKeyState(VK_F8) & 0x8000) != 0));
    return true;
}
