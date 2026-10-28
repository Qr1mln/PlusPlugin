// 取点共享状态：面板与事件弹窗共用一条数据流。
// 传输：优先用宿主进程（framework.dll）主动推送的 {"t":"cursor",...}（约 30Hz，前端不轮询）；
//       收不到推送时自动退回轮询 {"type":"cursor"}，保证旧 DLL 也能用。
// 只读：宿主侧用 GetCursorPos/ScreenToClient 报告光标位置，不注入任何事件。
import { reactive } from 'vue'
import { request, send, onMessage, onStatus } from './ws'

const POLL_MS = 70 // 兜底轮询间隔
const RETRY_MS = 500 // 未连接时退避，别把失败刷成风暴
const TIMEOUT_MS = 1200
const PUSH_GRACE_MS = 600 // 订阅后先给推送一点时间
const PUSH_ALIVE_MS = 3000 // 超过这么久没推送帧就认为推送断了

export const cursor = reactive({
  active: false,
  sample: null, // 最近一次光标读数（推送或轮询）
  frozen: null, // F8 捕获到的点
  picked: null, // 拾取模式返回的点（左键被吞掉）
  picking: false, // 是否处于拾取模式
  error: '', // 接口/连接错误
  scale: 1, // /api/app 拿到的 DPI 缩放，作 sample.scale 的兜底
})

let timer = null
let inFlight = false
let prevF8 = null // null = 还没采到第一帧，避免把“已按住”当成新的捕获
let pushSeenAt = 0 // 最近一次收到推送帧的时间戳

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

// 把读数塞进状态；F8 上升沿且光标在窗口内时冻结该点。
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

// 推送帧：cursor 光标流 / pick 拾取结果
function applyPush(obj) {
  if (obj.t === 'cursor') {
    pushSeenAt = Date.now()
    applySample(obj)
    return
  }
  if (obj.t !== 'pick') return
  if (obj.action === 'ready') {
    cursor.picking = true
    return
  }
  if (obj.action === 'cancel') {
    cursor.picking = false
    return
  }
  if (obj.action === 'failed') {
    cursor.picking = false
    cursor.error = obj.error || '拾取失败'
    return
  }
  cursor.picked = obj
  cursor.picking = false
}

// 只要订阅过一次就常驻监听；未取点时不发命令、不轮询。
onMessage((raw) => {
  if (!cursor.active) return
  let obj = null
  try {
    obj = JSON.parse(raw)
  } catch (_) {
    return
  }
  if (obj && obj.t) applyPush(obj)
})

// WS 重连后订阅会丢（服务端按连接记），这里补发一次。
onStatus((status) => {
  if (status === '已连接' && cursor.active) {
    pushSeenAt = 0
    send({ type: 'cursor', action: 'watch' })
  }
})

async function tick() {
  timer = null
  if (!cursor.active) return
  if (typeof document !== 'undefined' && document.hidden) return // 隐藏时暂停，重新可见后 resume 拉起
  // 推送还在跑就别轮询了，隔一会儿再看它是否还活着
  if (pushSeenAt && Date.now() - pushSeenAt < PUSH_ALIVE_MS) {
    schedule(PUSH_GRACE_MS)
    return
  }
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
  pushSeenAt = 0
  document.addEventListener('visibilitychange', onVisibility)
  send({ type: 'cursor', action: 'watch' }) // 订阅推送
  schedule(PUSH_GRACE_MS)
}

export function stop() {
  cursor.active = false
  clearTimer()
  send({ type: 'cursor', action: 'unwatch' })
  document.removeEventListener('visibilitychange', onVisibility)
  cursor.sample = null
  cursor.error = ''
  cursor.picking = false
}

export function setScale(scale) {
  const n = Number(scale)
  if (n > 0) cursor.scale = n
}

// 拾取模式：宿主装钩子，下一次落在应用窗口内的左键被吞掉并回传坐标。
export function beginPick() {
  cursor.picked = null
  cursor.picking = true
  send({ type: 'pick', action: 'start' })
}

export function cancelPick() {
  cursor.picking = false
  send({ type: 'pick', action: 'cancel' })
}

// 面板/弹窗共用的取值：优先冻结点/拾取点，否则用窗口内的实时点。
export function currentPoint() {
  if (cursor.frozen) return cursor.frozen
  if (cursor.picked) return cursor.picked
  if (cursor.sample && cursor.sample.inside) return cursor.sample
  return null
}

export function fmtCoord(v) {
  const n = Number(v) || 0
  return String(Math.round(n * 10) / 10)
}
