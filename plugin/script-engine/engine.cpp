// ============================================================================
// engine.cpp - 命令队列 + 执行线程 + 日志队列/日志线程 + 事件执行
//
// 线程模型（全部 std::thread）：
//   exec 线程   等命令队列（condition_variable）→ load/play/stop
//   log  线程   等日志队列 → 转发 host->log 并推给 TCP 客户端
//   两个线程都在 se::Shutdown 里 join，没有全局 HANDLE/CRITICAL_SECTION。
// ============================================================================
#include "engine.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "schedule.h"

#include "../../third_party/nlohmann/json.hpp"

namespace se {
namespace {

const Host* g_host = nullptr;
std::atomic<bool> g_quit{false};     // 引擎整体退出
std::atomic<bool> g_stop{false};     // 本轮执行请求停止
std::atomic<bool> g_running{false};  // 正在跑一轮（诊断用）

std::mutex g_cmdMx;
std::condition_variable g_cmdCv;
std::deque<Command> g_cmds;
std::vector<Event> g_events;
int g_loop = 1;

struct LogItem {
    bool raw;             // true = 已经是完整 JSON 帧，原样转发
    std::string payload;
};
std::mutex g_logMx;
std::condition_variable g_logCv;
std::deque<LogItem> g_logs;

std::thread g_execThread;
std::thread g_logThread;

std::string JsonLogFrame(const std::string& msg) {
    return nlohmann::json{{"t", "log"}, {"msg", msg}}.dump();
}

void LogLoop() {
    for (;;) {
        std::deque<LogItem> batch;
        {
            std::unique_lock<std::mutex> lk(g_logMx);
            g_logCv.wait(lk, [] { return g_quit.load() || !g_logs.empty(); });
            if (g_logs.empty()) {
                if (g_quit.load()) return;
                continue;
            }
            batch.swap(g_logs);
        }
        for (const LogItem& item : batch) {
            if (item.raw) {
                tcp::Send(item.payload);
            } else {
                if (g_host != nullptr && g_host->log != nullptr) {
                    g_host->log(item.payload.c_str());
                }
                tcp::Send(JsonLogFrame(item.payload));
            }
        }
    }
}

void RunEvent(const Event& ev, HWND view) {
    if (ev.type == "delay") {
        int left = ev.ms;
        while (left > 0) {
            if (g_stop.load()) return;
            const int chunk = (left < 50) ? left : 50;
            Sleep(static_cast<DWORD>(chunk));
            left -= chunk;
        }
        Log("delay: %d ms", ev.ms);
    } else if (ev.type == "input") {
        const int n = PP_InputText(view, ev.text.c_str());
        Log("input: %d chars (%s)", n, ev.text.c_str());
    } else if (ev.type == "key") {
        int vk = 0;
        bool ctrl = false, alt = false, shift = false, win = false;
        if (!ParseKeyCombo(ev.key, vk, ctrl, alt, shift, win)) {
            Log("key: unrecognized %s", ev.key.c_str());
            return;
        }
        const int action = (ev.action == "down") ? 1 : (ev.action == "up") ? 2 : 0;
        const int n = PP_Key(view, vk, ctrl ? 1 : 0, alt ? 1 : 0, shift ? 1 : 0,
                             win ? 1 : 0, action, 0);
        Log("key: %s -> %d", ev.key.c_str(), n);
    } else if (ev.type == "findImage") {
        const int rc = PP_FindImage(view, ev.image.c_str(), ev.threshold, ev.timeout_ms);
        Log("findImage: rc=%d (%s)", rc, ev.image.c_str());
    } else {  // 默认 mouse
        const int button = (ev.button == "right") ? 1 : (ev.button == "middle") ? 2 : 0;
        const int action = (ev.action == "move") ? 0 : (ev.action == "down") ? 1
                                                    : (ev.action == "up") ? 2 : 3;
        const float scale = (g_host != nullptr && g_host->scale != nullptr)
                                ? g_host->scale() : 1.0f;
        const int px = static_cast<int>(ev.x * scale + 0.5f);
        const int py = static_cast<int>(ev.y * scale + 0.5f);
        const int rc = PP_Mouse(view, px, py, button, action);
        Log("mouse: %s %s (%d,%d) rc=%d", ev.button.c_str(), ev.action.c_str(), px, py, rc);
    }
}

void RunLoop() {
    Log("run: ENTER");
    g_stop = false;
    g_running = true;
    HWND view = (g_host != nullptr && g_host->viewWindow != nullptr)
                    ? g_host->viewWindow() : nullptr;
    char cls[64] = {0};
    if (view != nullptr) GetClassNameA(view, cls, sizeof cls - 1);
    // 只有 FLUTTERVIEW 才收得到 PostMessage 的鼠标事件；若这里是
    // FLUTTER_RUNNER_WIN32_WINDOW，说明 Host::viewWindow 又退回了顶层窗口。
    Log("run: view=%p class=%s", static_cast<void*>(view), cls[0] ? cls : "?");
    if (view == nullptr || !IsWindow(view)) {
        Log("run: view window 不可用");
        SendRaw("{\"t\":\"done\"}");
        g_running = false;
        return;
    }
    const int total = static_cast<int>(g_events.size());
    const int loops = g_loop;
    Log("run: events=%d loop=%d", total, loops);
    bool stopped = false;
    for (int r = 1; r <= loops && !stopped; ++r) {
        for (int i = 0; i < total; ++i) {
            if (g_stop.load()) {
                stopped = true;
                break;
            }
            RunEvent(g_events[i], view);
        }
    }
    Log(stopped ? "stopped" : "done");
    SendRaw(stopped ? "{\"t\":\"stopped\"}" : "{\"t\":\"done\"}");
    g_running = false;
}

void ExecLoop() {
    for (;;) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lk(g_cmdMx);
            g_cmdCv.wait(lk, [] { return g_quit.load() || !g_cmds.empty(); });
            if (g_cmds.empty()) {
                if (g_quit.load()) return;
                continue;
            }
            cmd = std::move(g_cmds.front());
            g_cmds.pop_front();
        }
        switch (cmd.type) {
        case CmdType::Load:
            g_events = std::move(cmd.events);
            g_loop = cmd.loop;
            Log("loaded %d events, loop=%d", static_cast<int>(g_events.size()), g_loop);
            break;
        case CmdType::Stop:
            g_stop = true;
            Log("stop requested");
            break;
        case CmdType::Play:
            Log("exec: dispatch play");
            RunLoop();
            Log("exec: play returned");
            break;
        case CmdType::ReloadSchedule:
            sched::Reload();
            SendRaw(sched::StatusJson());   // 让编辑器立刻拿到新的"下次运行"
            break;
        case CmdType::GetSchedule:
            SendRaw(sched::StatusJson());
            break;
        case CmdType::RunScripts:
            sched::PushScripts(cmd.scripts, cmd.dir, cmd.loop, cmd.ms, "manual");
            break;
        case CmdType::Delay: {
            int left = cmd.ms;
            while (left > 0) {
                if (g_stop.load() || g_quit.load()) break;
                const int chunk = (left < 50) ? left : 50;
                Sleep(static_cast<DWORD>(chunk));
                left -= chunk;
            }
            Log("gap: %d ms", cmd.ms);
            break;
        }
        }
    }
}

}  // namespace

// -- 日志 -------------------------------------------------------------------
void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        if (g_logs.size() < 4096) g_logs.push_back({false, buf});
    }
    g_logCv.notify_one();
}

void SendRaw(const std::string& json_frame) {
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        if (g_logs.size() < 4096) g_logs.push_back({true, json_frame});
    }
    g_logCv.notify_one();
}

// -- 命令队列 ---------------------------------------------------------------
void Push(Command cmd) {
    {
        std::lock_guard<std::mutex> lk(g_cmdMx);
        g_cmds.push_back(std::move(cmd));
    }
    g_cmdCv.notify_one();
}

void RequestStop() {
    g_stop = true;
}

const Host* GetHost() {
    return g_host;
}

bool Quitting() {
    return g_quit.load();
}

// -- 生命周期 ---------------------------------------------------------------
bool Start(const Host* host) {
    if (host == nullptr) return false;
    g_host = host;
    g_quit = false;
    g_stop = false;
    g_running = false;
    try {
        g_execThread = std::thread(ExecLoop);
    } catch (...) {
        return false;
    }
    try {
        g_logThread = std::thread(LogLoop);
    } catch (...) {
        g_quit = true;
        g_cmdCv.notify_all();
        if (g_execThread.joinable()) g_execThread.join();
        return false;
    }
    return true;
}

void Shutdown() {
    g_quit = true;
    g_stop = true;
    g_cmdCv.notify_all();
    g_logCv.notify_all();
    if (g_execThread.joinable()) g_execThread.join();
    if (g_logThread.joinable()) g_logThread.join();
    {
        std::lock_guard<std::mutex> lk(g_cmdMx);
        g_cmds.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        g_logs.clear();
    }
    g_running = false;
    g_host = nullptr;
}

}  // namespace se
