// 取点共享状态：面板与事件弹窗共用一份轮询，避免两处各自打 /ws。
// 只读：宿主侧用 GetCursorPos/ScreenToClient 报告光标位置，不注入任何事件。
import { reactive } from 'vue'
import { request } from './ws'

const POLL_MS = 70 // 约 14 次/秒，肉眼连续
const RETRY_MS = 500 // 未连接时退避，别把失败刷成风暴
const TIMEOUT_MS = 1200

export const cursor = reactive({
  active: false,
  sample: null, // 宿主最近一次读数
  frozen: null, // F8 捕获到的点
  error: '', // 接口/连接错误
  scale: 1, // /api/app 拿到的 DPI 缩放，作 sample.scale 的兜底
})

let timer = null
let inFlight = false
let prevF8 = null // null = 还没采到第一帧，避免把“已按住”当成新的捕获

function schedule(ms) {
  if (timer != null) return
  timer = setTimeout(tick, ms)
}

function clearTimer() {
  if (timer != null) {
    clearTimeout(timer)
    timer = null
  }
}

// 把宿主回包塞进状态；F8 上升沿且光标在窗口内时冻结该点。
function applySample(data) {
  const s = {
    sx: Number(data.sx) || 0,
    sy: Number(data.sy) || 0,
    x: Number(data.x) || 0,
    y: Number(data.y) || 0,
    lx: Number(data.lx) || 0,
    ly: Number(data.ly) || 0,
    w: Number(data.w) || 0,
    h: Number(data.h) || 0,
    scale: Number(data.scale) || cursor.scale || 1,
    inside: !!data.inside,
  }
  const f8 = !!data.f8
  // 上升沿捕获；第一帧（prevF8 === null）只记键态，避免“已按住”被当成新捕获
  if (prevF8 !== null && f8 && !prevF8 && s.inside) {
    cursor.frozen = s
  }
  prevF8 = f8
  cursor.sample = s
}

async function tick() {
  timer = null
  if (!cursor.active) return
  if (typeof document !== 'undefined' && document.hidden) return // 隐藏时暂停，重新可见后 resume 拉起
  if (inFlight) {
    schedule(POLL_MS)
    return
  }
  inFlight = true
  try {
    const res = await request({ type: 'cursor' }, TIMEOUT_MS)
    if (!cursor.active) return // 请求在 stop() 之后才回来：丢弃，别把 sample 复活
    applySample(res && res.data ? res.data : {})
    cursor.error = ''
    schedule(POLL_MS)
  } catch (e) {
    // 失败保留上一帧读数，只标记状态，不打断用户操作
    cursor.error = e && e.message === 'timeout' ? '取点超时' : '未连接'
    schedule(RETRY_MS)
  } finally {
    inFlight = false
  }
}

function onVisibility() {
  if (!cursor.active) return
  if (document.hidden) {
    clearTimer()
  } else {
    schedule(0)
  }
}

export function start() {
  if (cursor.active) return
  cursor.active = true
  cursor.error = ''
  prevF8 = null // 重新采第一帧键态，避免上一轮残留被当成新的捕获
  document.addEventListener('visibilitychange', onVisibility)
  schedule(0)
}

export function stop() {
  cursor.active = false
  clearTimer()
  document.removeEventListener('visibilitychange', onVisibility)
  cursor.sample = null
  cursor.error = ''
}

export function setScale(scale) {
  const n = Number(scale)
  if (n > 0) cursor.scale = n
}

// 面板/弹窗共用的取值：优先冻结点，否则用窗口内的实时点。
export function currentPoint() {
  if (cursor.frozen) return cursor.frozen
  if (cursor.sample && cursor.sample.inside) return cursor.sample
  return null
}

export function fmtCoord(v) {
  const n = Number(v) || 0
  return String(Math.round(n * 10) / 10)
}
