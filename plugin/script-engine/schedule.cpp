// ============================================================================
// schedule.cpp - 定时任务调度线程
//
// 结构：一份内存任务表（读自 schedule.json）+ 一个 std::thread。
// 循环每轮算出"最近的下次触发"并 cv.wait_until 到那时；最长只等 30s，
// 用来兜住系统休眠/时间回拨/配置热更新，醒来重算即可。
// ============================================================================
#include "schedule.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"

#include "../../third_party/nlohmann/json.hpp"

namespace sched {
namespace {

using Clock = std::chrono::system_clock;

struct Job {
    std::string id;
    std::string name;
    std::vector<std::string> scripts;   // 按顺序执行；支持多脚本
    std::string dir;                    // 脚本目录（留空 = scripts 根目录）
    bool enabled = true;
    int loop = 1;
    int gap_ms = 0;                     // 每个脚本跑完后、下一个脚本开始前的间隔

    std::string type;          // daily | interval | once
    int hour = 0;              // daily
    int minute = 0;            // daily
    std::vector<int> days;     // daily: 1..7（周一..周日），空 = 每天
    int minutes = 30;          // interval
    std::string once_at;       // once: "YYYY-MM-DD HH:MM"

    std::time_t next = 0;      // 0 = 无下次
    bool fired = false;        // once 已触发
};

std::mutex g_mx;
std::condition_variable g_cv;
std::vector<Job> g_jobs;
bool g_quit = false;
bool g_loaded = false;
std::thread g_thread;

// localtime 的静态缓冲不是线程安全的；调度线程（算下次触发）和执行线程
// （get_schedule 生成状态）都会用到，所以统一走一把锁，避免 MSVC/mingw 的
// localtime_s / localtime_r 签名差异。
std::mutex g_tmMx;

void LocalTime(const std::time_t* t, std::tm* out) {
    std::lock_guard<std::mutex> lk(g_tmMx);
    const std::tm* tmp = std::localtime(t);
    *out = (tmp != nullptr) ? *tmp : std::tm{};
}

std::filesystem::path TtVoiceDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf) / L"TTVoice";
}

std::filesystem::path ConfigPath() {
    const std::filesystem::path dir = TtVoiceDir();
    return dir.empty() ? std::filesystem::path() : dir / L"schedule.json";
}

// UTF-8 -> 宽字符（脚本名/目录名可能是中文，不能按字节直接转）
std::wstring WidenUtf8(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], len);
    return out;
}

// 任务可选 dir：留空 = %APPDATA%\TTVoice\scripts；相对路径相对该根目录，绝对路径直接用。
std::filesystem::path ScriptDirRoot() {
    const std::filesystem::path base = TtVoiceDir();
    return base.empty() ? std::filesystem::path() : base / L"scripts";
}

std::filesystem::path ResolveScriptDir(const std::string& dir) {
    const std::filesystem::path root = ScriptDirRoot();
    if (dir.empty()) return root;
    const std::filesystem::path given = WidenUtf8(dir);
    if (given.empty()) return root;
    if (given.is_absolute()) return given;
    if (root.empty()) return {};
    return root / given;
}

std::filesystem::path ScriptPath(const std::string& name, const std::string& dir) {
    if (name.empty()) return {};
    const std::filesystem::path base = ResolveScriptDir(dir);
    if (base.empty()) return {};
    std::wstring file = WidenUtf8(name);
    for (wchar_t& c : file) {
        if (c == L'/' || c == L'\\' || c == L':') c = L'_';   // 文件名里不放分隔符
    }
    if (file.size() > 5 && file.compare(file.size() - 5, 5, L".json") == 0) {
        file.erase(file.size() - 5);                          // 允许写 xxx.json
    }
    return base / (file + L".json");
}

std::string FormatTime(std::time_t t) {
    if (t == 0) return "";
    std::tm tmv = {};
    LocalTime(&t, &tmv);
    char buf[32] = {};
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

int ParseHhMm(const std::string& text, int& hour, int& minute) {
    int h = 0, m = 0;
    if (sscanf(text.c_str(), "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    hour = h;
    minute = m;
    return 0;
}

std::time_t ParseOnce(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    std::tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    return std::mktime(&tmv);
}

// 解析脚本文件（与编辑器保存的格式一致），组合成 load 命令。
bool ParseScript(const std::string& text, se::Command& out) {
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        out.type = se::CmdType::Load;
        out.loop = j.value("loop", 1);
        out.events.clear();
        const auto it = j.find("events");
        if (it == j.end() || !it->is_array()) return true;
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            se::Event ev;
            ev.type = e.value("type", std::string());
            ev.action = e.value("action", std::string());
            ev.button = e.value("button", std::string());
            ev.text = e.value("text", std::string());
            ev.key = e.value("key", std::string());
            ev.image = e.value("image", std::string());
            ev.x = e.value("x", 0);
            ev.y = e.value("y", 0);
            ev.ms = e.value("ms", 0);
            ev.timeout_ms = e.value("timeout_ms", 0);
            ev.threshold = e.value("threshold", 0.9);
            out.events.push_back(std::move(ev));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// daily 的下一次触发：从今天起最多看 8 天，取第一个满足星期且时间在未来的点。
std::time_t NextDaily(const Job& job, std::time_t now) {
    std::tm base = {};
    LocalTime(&now, &base);
    for (int add = 0; add <= 8; ++add) {
        std::tm cand = base;
        cand.tm_mday += add;
        cand.tm_hour = job.hour;
        cand.tm_min = job.minute;
        cand.tm_sec = 0;
        cand.tm_isdst = -1;
        const std::time_t when = std::mktime(&cand);   // 归一化 + 处理 DST
        if (when == static_cast<std::time_t>(-1)) continue;
        if (when <= now) continue;
        if (!job.days.empty()) {
            std::tm norm = {};
            LocalTime(&when, &norm);
            const int iso = (norm.tm_wday == 0) ? 7 : norm.tm_wday;   // 1..7
            if (std::find(job.days.begin(), job.days.end(), iso) == job.days.end()) continue;
        }
        return when;
    }
    return 0;
}

void ComputeNext(Job& job, std::time_t now) {
    const std::string type = job.type;
    if (type == "interval") {
        job.next = now + static_cast<std::time_t>(std::max(1, job.minutes)) * 60;
    } else if (type == "once") {
        if (job.fired) {
            job.next = 0;
            return;
        }
        const std::time_t at = ParseOnce(job.once_at);
        job.next = (at > now) ? at : 0;
    } else {   // daily
        job.next = NextDaily(job, now);
    }
}

Job ParseJob(const nlohmann::json& j) {
    Job job;
    job.id = j.value("id", std::string());
    job.name = j.value("name", std::string());

    // scripts 是当前格式（按顺序执行多个脚本）；script 是早期的单脚本字段。
    const auto scripts = j.find("scripts");
    if (scripts != j.end() && scripts->is_array()) {
        for (const auto& item : *scripts) {
            if (item.is_string()) {
                const std::string name = item.get<std::string>();
                if (!name.empty()) job.scripts.push_back(name);
            }
        }
    }
    if (job.scripts.empty()) {
        const std::string single = j.value("script", std::string());
        if (!single.empty()) job.scripts.push_back(single);
    }

    job.dir = j.value("dir", std::string());
    job.enabled = j.value("enabled", true);
    job.loop = std::max(1, j.value("loop", 1));
    job.gap_ms = std::max(0, j.value("gap_ms", 0));
    const auto when = j.find("when");
    if (when != j.end() && when->is_object()) {
        job.type = when->value("type", std::string("daily"));
        if (job.type == "daily") {
            ParseHhMm(when->value("at", std::string("00:00")), job.hour, job.minute);
            const auto days = when->find("days");
            if (days != when->end() && days->is_array()) {
                for (const auto& d : *days) {
                    if (d.is_number_integer()) {
                        const int v = d.get<int>();
                        if (v >= 1 && v <= 7) job.days.push_back(v);
                    }
                }
            }
        } else if (job.type == "interval") {
            job.minutes = std::max(1, when->value("minutes", 30));
        } else if (job.type == "once") {
            job.once_at = when->value("at", std::string());
        } else {
            job.type = "daily";   // 未知类型退化成每天 00:00 之外的空设置
        }
    }
    return job;
}

void LoadConfigLocked() {
    const std::filesystem::path path = ConfigPath();
    g_jobs.clear();
    g_loaded = true;
    if (path.empty()) {
        se::Log("sched: 无法定位 %%APPDATA%%，定时任务未加载");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        se::Log("sched: 没有 %ls，定时任务为空", path.c_str());
        return;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            se::Log("sched: schedule.json 解析失败，定时任务为空");
            return;
        }
        const auto jobs = j.find("jobs");
        if (jobs == j.end() || !jobs->is_array()) {
            se::Log("sched: schedule.json 没有 jobs 数组");
            return;
        }
        for (const auto& item : *jobs) {
            if (!item.is_object()) continue;
            Job job = ParseJob(item);
            if (job.scripts.empty()) continue;
            g_jobs.push_back(std::move(job));
        }
    } catch (const std::exception&) {
        se::Log("sched: schedule.json 解析异常，定时任务为空");
        g_jobs.clear();
        return;
    }

    const std::time_t now = std::time(nullptr);
    int active = 0;
    for (Job& job : g_jobs) {
        if (job.enabled) {
            ComputeNext(job, now);
            if (job.next != 0) ++active;
        }
    }
    se::Log("sched: 已加载 %d 个任务（%d 个待触发）", static_cast<int>(g_jobs.size()), active);
}

// 读一个脚本文件并压一对 load+play。执行线程串行消费命令队列，所以后一个脚本
// 一定等前一个跑完才开始。tag 只用于日志（job=t1 / run=manual）。
bool PushOneScript(const std::string& name, const std::string& dir, int loop,
                   const std::string& tag, int step, int total) {
    const std::filesystem::path path = ScriptPath(name, dir);
    if (path.empty()) {
        se::Log("sched: %s 第 %d/%d 个脚本名无效", tag.c_str(), step, total);
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        se::Log("sched: %s 第 %d/%d 个脚本找不到（%ls）", tag.c_str(), step, total, path.c_str());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    se::Command load;
    if (!ParseScript(text, load)) {
        se::Log("sched: %s 第 %d/%d 个脚本解析失败（%ls）", tag.c_str(), step, total,
                path.c_str());
        return false;
    }
    if (loop > 1) load.loop = loop;

    se::Log("sched: run %s step=%d/%d script=%s events=%d loop=%d", tag.c_str(), step, total,
            name.c_str(), static_cast<int>(load.events.size()), load.loop);
    se::Push(std::move(load));
    se::Command play;
    play.type = se::CmdType::Play;
    se::Push(std::move(play));
    return true;
}

// 脚本之间的间隔：压一个 Delay 命令，由执行线程按需睡眠（可被停止打断）。
void PushGap(int gap_ms) {
    if (gap_ms <= 0) return;
    se::Command wait;
    wait.type = se::CmdType::Delay;
    wait.ms = gap_ms;
    se::Push(std::move(wait));
}

bool FireJob(Job& job) {
    if (job.scripts.empty()) {
        se::Log("sched: 任务 %s 没有脚本", job.id.c_str());
        return false;
    }
    const std::string tag = "job=" + job.id;
    const int total = static_cast<int>(job.scripts.size());
    bool pushed = false;
    for (int i = 0; i < total; ++i) {
        if (PushOneScript(job.scripts[i], job.dir, job.loop, tag, i + 1, total)) {
            pushed = true;
            if (i + 1 < total) PushGap(job.gap_ms);   // 最后一个脚本后不加间隔
        }
    }
    if (!pushed) {
        se::Log("sched: 任务 %s 没有可执行的脚本", job.id.c_str());
    }
    return pushed;
}

void Loop() {
    std::unique_lock<std::mutex> lk(g_mx);
    while (!g_quit) {
        if (!g_loaded) {
            LoadConfigLocked();
        }
        const std::time_t now = std::time(nullptr);

        // 到期的先跑（可能是休眠后一次醒来的多个任务）。
        std::vector<Job*> due;
        for (Job& job : g_jobs) {
            if (job.enabled && job.next != 0 && job.next <= now) {
                due.push_back(&job);
            }
        }
        if (!due.empty()) {
            lk.unlock();
            for (Job* job : due) {
                FireJob(*job);
                if (job->type == "once") {
                    job->fired = true;
                }
            }
            lk.lock();
            const std::time_t after = std::time(nullptr);
            for (Job* job : due) {
                ComputeNext(*job, after);
            }
            continue;
        }

        std::time_t next = 0;
        for (const Job& job : g_jobs) {
            if (job.enabled && job.next != 0 && (next == 0 || job.next < next)) {
                next = job.next;
            }
        }
        const auto now_tp = Clock::now();
        if (next == 0) {
            // 没有待触发任务：30s 后重算（配置可能被 reload 或系统时间变了）。
            g_cv.wait_for(lk, std::chrono::seconds(30));
            continue;
        }
        const auto fire_tp = now_tp + std::chrono::seconds(next - now);
        const auto cap = now_tp + std::chrono::seconds(30);
        g_cv.wait_until(lk, (fire_tp < cap) ? fire_tp : cap);
    }
}

}  // namespace

bool Start() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_quit = false;
        g_loaded = false;
    }
    try {
        g_thread = std::thread(Loop);
    } catch (...) {
        se::Log("sched: 调度线程启动失败");
        return false;
    }
    return true;
}

void Stop() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_quit = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void Reload() {
    // 同步重读：调用方（get/reload 命令）紧接着要 StatusJson，必须看到新配置。
    {
        std::lock_guard<std::mutex> lk(g_mx);
        LoadConfigLocked();
    }
    g_cv.notify_all();
    se::Log("sched: 配置已重新加载");
}

void PushScripts(const std::vector<std::string>& scripts, const std::string& dir, int loop,
                 int gap_ms, const char* tag) {
    if (scripts.empty()) {
        se::Log("sched: 立即执行失败：没有脚本");
        return;
    }
    const std::string label = std::string("run=") + ((tag != nullptr && tag[0] != 0) ? tag : "manual");
    const int total = static_cast<int>(scripts.size());
    int ok = 0;
    for (int i = 0; i < total; ++i) {
        if (PushOneScript(scripts[i], dir, loop, label, i + 1, total)) {
            ++ok;
            if (i + 1 < total) PushGap(gap_ms);
        }
    }
    se::Log("sched: 立即执行 %d/%d 个脚本（dir=%s, gap=%dms）", ok, total,
            dir.empty() ? "默认" : dir.c_str(), gap_ms);
}

std::string StatusJson() {
    std::lock_guard<std::mutex> lk(g_mx);
    nlohmann::json out;
    out["t"] = "schedule";
    out["now"] = FormatTime(std::time(nullptr));
    nlohmann::json jobs = nlohmann::json::array();
    std::time_t next = 0;
    std::string next_id;
    for (const Job& job : g_jobs) {
        nlohmann::json item;
        item["id"] = job.id;
        item["name"] = job.name;
        item["scripts"] = job.scripts;
        item["dir"] = job.dir;
        item["gap_ms"] = job.gap_ms;
        item["enabled"] = job.enabled;
        item["type"] = job.type;
        item["next"] = FormatTime(job.next);
        jobs.push_back(std::move(item));
        if (job.enabled && job.next != 0 && (next == 0 || job.next < next)) {
            next = job.next;
            next_id = job.id;
        }
    }
    out["jobs"] = std::move(jobs);
    out["next_id"] = next_id;
    out["next"] = FormatTime(next);
    return out.dump();
}

}  // namespace sched
