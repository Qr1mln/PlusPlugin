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
