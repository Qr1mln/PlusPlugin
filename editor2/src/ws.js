import ReconnectingWebSocket from 'reconnecting-websocket'

let socket = null
const statusListeners = []
const messageListeners = []
const pending = new Map() // id -> { resolve, reject }
let seq = 0

function emitStatus(text) {
  for (const fn of statusListeners) fn(text)
}

function emitMessage(data) {
  for (const fn of messageListeners) fn(data)
}

// 页面由 framework.dll 托管时是同源；Vite dev(5173) 时由 vite proxy 转发 /ws。
export function wsUrl() {
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${window.location.host}/ws`
}

export function connect() {
  if (socket) return socket
  socket = new ReconnectingWebSocket(wsUrl(), [], {
    minReconnectionDelay: 500,
    maxReconnectionDelay: 5000,
    reconnectionDelayGrowFactor: 1.5,
    maxRetries: Infinity,
  })
  socket.addEventListener('open', () => emitStatus('已连接'))
  socket.addEventListener('close', () => {
    for (const [, p] of pending) p.reject(new Error('closed'))
    pending.clear()
    emitStatus('未连接')
  })
  socket.addEventListener('error', () => emitStatus('连接错误'))
  socket.addEventListener('message', (e) => {
    let obj = null
    try {
      obj = JSON.parse(e.data)
    } catch (_) {
      // 非 JSON：交给普通消息监听
    }
    if (obj && obj.id && pending.has(obj.id)) {
      const p = pending.get(obj.id)
      pending.delete(obj.id)
      if (obj.success) p.resolve(obj)
      else p.reject(new Error(obj.data || 'failed'))
      return
    }
    emitMessage(e.data)
  })
  return socket
}

export function send(obj) {
  if (!socket || socket.readyState !== WebSocket.OPEN) return false
  socket.send(typeof obj === 'string' ? obj : JSON.stringify(obj))
  return true
}

// 发一条命令并等待同 id 的回包；成功 resolve(回包)，失败 reject(Error)。
export function request(obj, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    if (
      !socket ||
      (socket.readyState !== WebSocket.OPEN && socket.readyState !== WebSocket.CONNECTING)
    ) {
      reject(new Error('ws not connected'))
      return
    }
    const id = String(obj.id || ++seq)
    const msg = { ...obj, id }
    const timer = setTimeout(() => {
      pending.delete(id)
      reject(new Error('timeout'))
    }, timeoutMs)
    pending.set(id, {
      resolve: (v) => {
        clearTimeout(timer)
        resolve(v)
      },
      reject: (e) => {
        clearTimeout(timer)
        reject(e)
      },
    })
    socket.send(JSON.stringify(msg))
  })
}

export function onStatus(fn) {
  statusListeners.push(fn)
}

export function onMessage(fn) {
  messageListeners.push(fn)
}
