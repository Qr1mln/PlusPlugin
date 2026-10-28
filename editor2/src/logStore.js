// 操作日志：整个编辑器共用一份（App 的日志面板、事件弹窗、取点模块都往里写）。
// 只做内存环形缓冲（最多 500 行），不落盘：刷新页面即清空。
import { reactive } from 'vue'

const MAX_LINES = 500

export const logs = reactive([])

function stamp() {
  const d = new Date()
  const p = (n, w = 2) => String(n).padStart(w, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`
}

// level: debug / info / warn / error
export function pushLog(level, msg) {
  logs.push({ time: stamp(), level, msg })
  while (logs.length > MAX_LINES) {
    logs.shift()
  }
}

export function clearLogs() {
  logs.splice(0, logs.length)
}
