// ============================================================================
// schedule.h - 定时任务调度（进程内，与 TCP/编辑器无关）
//
// 配置：%APPDATA%\TTVoice\schedule.json（编辑器编辑，本模块只读）
//   {"version":1,"jobs":[
//     {"id":"j1","name":"每天上麦","script":"完成一次任意房间内上麦",
//      "enabled":true,"loop":1,"when":{"type":"daily","at":"21:30","days":[1,2,3,4,5]}},
//     {"id":"j2","name":"每30分钟","script":"xxx","enabled":true,
//      "when":{"type":"interval","minutes":30}},
//     {"id":"j3","name":"一次性","script":"xxx","enabled":true,
//      "when":{"type":"once","at":"2026-10-27 21:30"}}]}
//
// 语义（已定）：
//   - 到点把脚本作为 load+play 命令压进引擎队列；引擎正在跑时命令排队，
//     等本轮结束自动执行（不丢弃）。
//   - 不补跑：应用重启/休眠唤醒后只从"下一次未来时刻"开始。
//   - days 为 1..7（周一..周日），空表示每天；时间用本地时间。
//   - once 触发一次后本次进程内不再重复（reload 配置可复位）。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace sched {

bool Start();        // 读配置并起调度线程
void Stop();         // 停线程
void Reload();       // 重读 schedule.json（线程安全，立即生效）
std::string StatusJson();   // {"t":"schedule","now":"...","jobs":[...],"next":{...}}

// 立即执行一组脚本（定时任务的「立即执行」）：按顺序把 load+play 压进命令队列，
// 与定时触发走同一条执行路径，所以同样遵守"当前脚本跑完再开始"。
void PushScripts(const std::vector<std::string>& scripts, const std::string& dir, int loop,
                 int gap_ms, const char* tag);

}  // namespace sched
