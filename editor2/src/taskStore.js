import { ref } from 'vue'
import { request } from './ws'

// 任务列表（由 HTTP 轮询刷新）。元素：{id,name,status,loop_cur,loop_total,idx,count}
export const tasks = ref([])
let timer = null

// 拉取任务状态（GET /api/tasks）。
export async function refreshTasks() {
  try {
    const res = await fetch('/api/tasks')
    const j = await res.json()
    if (j && j.success && j.data && Array.isArray(j.data.tasks)) {
      tasks.value = j.data.tasks
    }
  } catch (_) {
    // 网络/解析错误忽略，保持上一次结果
  }
}

// 启动轮询（默认每 1.5s 刷新一次）。
export function startPolling(ms = 1500) {
  stopPolling()
  refreshTasks()
  timer = setInterval(refreshTasks, ms)
}

export function stopPolling() {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}

// 任务操作走 WS（type=task），复用现有 request/封套。
export async function startTask(name, loop) {
  return request({ type: 'task', action: 'start', name, loop: Number(loop) || 0 })
}
export async function stopTask(id) {
  return request({ type: 'task', action: 'stop', task: id })
}
export async function pauseTask(id) {
  return request({ type: 'task', action: 'pause', task: id })
}
export async function resumeTask(id) {
  return request({ type: 'task', action: 'resume', task: id })
}

// ---- 定时任务（schedule.json，由 DLL 只读并触发）----
// 元素：{id,name,enabled,loop,gap_ms,dir,scripts:[...],when:{type,at,minutes,days},next,next_run,running,pending}
export const jobs = ref([])

export async function getSchedule() {
  try {
    const res = await fetch('/api/schedule')
    const j = await res.json()
    if (j && j.success && j.data && Array.isArray(j.data.jobs)) {
      jobs.value = j.data.jobs
    }
    return j
  } catch (_) {
    return null
  }
}

// 只读取服务器上的任务列表，不覆盖本地 jobs（弹窗轮询合并运行状态用）。
export async function fetchScheduleRaw() {
  try {
    const res = await fetch('/api/schedule')
    const j = await res.json()
    if (j && j.success && j.data && Array.isArray(j.data.jobs)) return j.data.jobs
    return null
  } catch (_) {
    return null
  }
}

// 保存整份配置到 schedule.json 并让引擎重新加载。
export async function saveSchedule(list) {
  try {
    const res = await fetch('/api/schedule', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ jobs: list }),
    })
    const j = await res.json()
    if (j && j.success && j.data && Array.isArray(j.data.jobs)) {
      jobs.value = j.data.jobs
    } else if (j && j.success) {
      await getSchedule()
    }
    return j
  } catch (_) {
    return null
  }
}

// 立即执行某任务（不等触发时刻，仍受单任务约束：忙则排队）。
export async function runScheduleNow(id) {
  try {
    const res = await fetch('/api/schedule?job=' + encodeURIComponent(id), { method: 'POST' })
    return await res.json()
  } catch (_) {
    return null
  }
}

// 重新加载 schedule.json（外部改动文件后）。
export async function reloadSchedule() {
  try {
    const res = await fetch('/api/schedule?action=reload', { method: 'POST' })
    const j = await res.json()
    if (j && j.success) await getSchedule()
    return j
  } catch (_) {
    return null
  }
}
