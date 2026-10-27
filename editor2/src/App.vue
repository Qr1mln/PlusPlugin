<script setup>
import { computed, nextTick, onMounted, ref, watch } from 'vue'
import { connect, onMessage, onStatus, request } from './ws'
import EventDialog from './EventDialog.vue'
import { setScale } from './cursorStore'

// ---- 演示数据（真实数据后续从脚本文件加载）----
const scripts = ref([])
const currentScript = ref('')

// 全局控件尺寸：small / medium / large / xlarge（默认 中），持久化到 localStorage
const SIZE_KEY = 'editor2.controlSize'
const SIZE_VALUES = ['small', 'medium', 'large', 'xlarge']

function loadSize() {
  try {
    const saved = localStorage.getItem(SIZE_KEY)
    if (SIZE_VALUES.includes(saved)) {
      return saved
    }
  } catch (_) {
    // localStorage 不可用时回退默认
  }
  return 'medium'
}

const size = ref(loadSize())
const sizeLabel = computed(
  () => ({ small: '小', medium: '中', large: '大', xlarge: '超大' })[size.value] || size.value,
)
watch(size, (value) => {
  try {
    localStorage.setItem(SIZE_KEY, value)
  } catch (_) {
    // 忽略持久化失败
  }
})

// 连接状态（占位，未接 WS）
const status = ref('未连接')

// 目标应用信息（“刷新应用”时从 /api/app 读回窗口几何）
const appInfo = ref('TTVoice.exe view -')
// DPI 缩放：脚本存“逻辑坐标”，PostMessage 需要物理客户区像素
const appScale = ref(1)

// 事件编辑弹窗 / 行选中
const dlgOpen = ref(false)
const dlgTitle = ref('事件')
const dlgInitial = ref({})
const dlgIndex = ref(-1)
const selectedRow = ref(-1)

const rows = ref([
  { type: '鼠标', params: '300,550', action: '左键单击', wheel: '0', note: '这是A操作。' },
  { type: '鼠标', params: '900,300', action: '左键双击', wheel: '0', note: '这是B操作。' },
  { type: '延迟', params: '1000', action: '', wheel: '', note: '' },
])

// 当前脚本的原始 events（保存时写回，避免丢失 json 字段）
const rawEvents = ref([])

const logs = ref([
  { time: '09:05:23.978', level: 'debug', msg: '' },
  { time: '09:05:23.978', level: 'info', msg: '' },
  { time: '09:05:23.978', level: 'error', msg: '' },
])

// 日志面板：默认贴底滚动；用户往上翻时暂停自动滚动
const logEl = ref(null)
let stickBottom = true

function onLogScroll() {
  const el = logEl.value
  if (!el) return
  stickBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 8
}

watch(
  () => logs.value.length,
  async () => {
    await nextTick()
    const el = logEl.value
    if (el && stickBottom) {
      el.scrollTop = el.scrollHeight
    }
  },
)

// ---- 所有实际方法先不实现：仅留占位，后续补业务 ----
function stub(name) {
  // TODO: 实现 name
  console.log('[TODO]', name)
}

// 顶部：脚本文件
// 脚本存于 %APPDATA%\TTVoice\scripts\*.json，由 framework.dll 的
// GET /api/scripts 提供（开发期经 Vite 代理到 127.0.0.1:47801）。
async function onRefreshScripts() {
  try {
    const res = await fetch('/api/scripts')
    const j = await res.json()
    if (j && j.success && Array.isArray(j.data)) {
      scripts.value = j.data
      if (!j.data.includes(currentScript.value)) {
        currentScript.value = j.data[0] || '' // 触发 watch，加载新脚本
      } else {
        await loadScript(currentScript.value) // 值没变，手动重读内容
      }
    } else {
      console.warn('[scripts] 返回异常', j)
    }
  } catch (e) {
    console.warn('[scripts] 读取失败', e)
  }
}

onMounted(() => {
  onRefreshScripts()
  onRefreshApp()

  // WebSocket（reconnecting-websocket，自带断线重连）
  onStatus((s) => {
    status.value = s
  })
  onMessage((data) => {
    try {
      console.log('[ws]', JSON.parse(data))
    } catch (_) {
      console.log('[ws]', data)
    }
  })
  connect()
})

// ---- 读取脚本内容并填充表格 ----
const TYPE_LABELS = { mouse: '鼠标', delay: '延迟', findImage: '找图', input: '输入', key: '按键' }
const MOUSE_ACTIONS = { click: '单击', move: '移动', down: '按下', up: '抬起' }
const MOUSE_BUTTONS = { left: '左键', right: '右键', middle: '中键' }
const KEY_ACTIONS = { press: '单击', down: '按下', up: '抬起' }

function toRow(ev) {
  const t = ev.type || 'mouse'
  let params = ''
  let action = ''
  let wheel = ''
  if (t === 'mouse') {
    params = `${ev.x ?? 0},${ev.y ?? 0}`
    action = (MOUSE_BUTTONS[ev.button] || '左键') + (MOUSE_ACTIONS[ev.action] || '单击')
    wheel = ev.wheel ?? 0
  } else if (t === 'delay') {
    params = `${ev.ms ?? 0}`
  } else if (t === 'findImage') {
    params = ev.image || ''
  } else if (t === 'input') {
    params = ev.text || ''
  } else if (t === 'key') {
    params = ev.key || ''
    action = KEY_ACTIONS[ev.action] || '单击'
  }
  return {
    type: TYPE_LABELS[t] || t,
    params: String(params),
    action,
    wheel: String(wheel),
    note: ev.remark || '',
  }
}

// 脚本文件：%APPDATA%\TTVoice\scripts\<name>.json
// 内容 {version,loop,events:[...]}（也兼容纯数组）。
async function loadScript(name) {
  if (!name) {
    rows.value = []
    return
  }
  try {
    const res = await fetch('/api/scripts?name=' + encodeURIComponent(name))
    const j = await res.json()
    if (j && j.success) {
      const data = j.data
      const events = Array.isArray(data)
        ? data
        : (data && Array.isArray(data.events) ? data.events : [])
      rawEvents.value = events
      rows.value = events.map(toRow)
    } else {
      rows.value = []
      console.warn('[script] 读取失败', j)
    }
  } catch (e) {
    rows.value = []
    console.warn('[script] 读取失败', e)
  }
}

watch(currentScript, (name) => {
  loadScript(name)
})
// 新建：创建空脚本并选中
async function onNewScript() {
  const name = window.prompt('新建脚本名称')
  if (!name) return
  try {
    const res = await fetch('/api/scripts?name=' + encodeURIComponent(name), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version: 2, name, loop: 1, events: [] }),
    })
    const j = await res.json()
    if (j && j.success) {
      await onRefreshScripts()
      currentScript.value = name
    } else {
      console.warn('[new] 失败', j)
    }
  } catch (e) {
    console.warn('[new] 失败', e)
  }
}

// 保存：把当前脚本的原始 events 写回文件
async function onSaveScript() {
  const name = currentScript.value
  if (!name) return
  try {
    const res = await fetch('/api/scripts?name=' + encodeURIComponent(name), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ version: 2, name, loop: 1, events: rawEvents.value }),
    })
    const j = await res.json()
    if (!j || !j.success) {
      console.warn('[save] 失败', j)
    }
  } catch (e) {
    console.warn('[save] 失败', e)
  }
}

// 改名：读旧 -> 以新名字保存 -> 删除旧文件
async function onRenameScript() {
  const oldName = currentScript.value
  if (!oldName) return
  const input = window.prompt('新脚本名称', oldName)
  if (input == null) return
  const newName = input.trim()
  if (!newName || newName === oldName) return
  if (scripts.value.includes(newName)) {
    pushLog('warn', `已存在同名脚本：${newName}`)
    return
  }
  try {
    const r1 = await fetch('/api/scripts?name=' + encodeURIComponent(oldName))
    const j1 = await r1.json()
    if (!j1 || !j1.success) {
      pushLog('error', '改名失败：读不到原脚本')
      return
    }
    const data = j1.data || {}
    const events = Array.isArray(data)
      ? data
      : data && Array.isArray(data.events)
        ? data.events
        : []
    const payload = {
      version: data.version || 2,
      name: newName,
      loop: data.loop || 1,
      events,
    }
    const r2 = await fetch('/api/scripts?name=' + encodeURIComponent(newName), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    })
    const j2 = await r2.json()
    if (!j2 || !j2.success) {
      pushLog('error', '改名失败：写入新脚本失败')
      return
    }
    const r3 = await fetch('/api/scripts?name=' + encodeURIComponent(oldName), {
      method: 'DELETE',
    })
    const j3 = await r3.json()
    if (!j3 || !j3.success) {
      pushLog('warn', '新脚本已生成，但删除旧脚本失败')
    }
    await onRefreshScripts()
    currentScript.value = newName
    pushLog('info', `已改名：${oldName} → ${newName}`)
  } catch (e) {
    pushLog('error', `改名失败：${e.message}`)
  }
}

// 删除：确认后删文件并刷新列表
async function onDeleteScript() {
  const name = currentScript.value
  if (!name) return
  if (!window.confirm(`删除脚本「${name}」？`)) return
  try {
    const res = await fetch('/api/scripts?name=' + encodeURIComponent(name), {
      method: 'DELETE',
    })
    const j = await res.json()
    if (j && j.success) {
      rows.value = []
      rawEvents.value = []
      await onRefreshScripts()
    } else {
      console.warn('[delete] 失败', j)
    }
  } catch (e) {
    console.warn('[delete] 失败', e)
  }
}

// ---- 试跑：把脚本 events 逐条经 WS 发给 /ws ----
const VK_MAP = {
  backspace: 8, tab: 9, enter: 13, return: 13, pause: 19, capslock: 20,
  esc: 27, escape: 27, space: 32, pageup: 33, pagedown: 34, end: 35, home: 36,
  left: 37, up: 38, right: 39, down: 40, printscreen: 44, insert: 45,
  delete: 46, del: 46, numlock: 144, scrolllock: 145,
}
for (let i = 1; i <= 24; i++) VK_MAP[`f${i}`] = 0x6f + i
for (const c of 'abcdefghijklmnopqrstuvwxyz') VK_MAP[c] = c.toUpperCase().charCodeAt(0)
for (const d of '0123456789') VK_MAP[d] = d.charCodeAt(0)

function resolveVk(name) {
  const key = String(name || '').trim()
  if (!key) return 0
  const low = key.toLowerCase()
  if (VK_MAP[low]) return VK_MAP[low]
  if (key.length === 1) return key.toUpperCase().charCodeAt(0)
  return 0
}

// 脚本事件 -> /ws 命令；返回 null 表示当前协议不支持。
function toCommand(ev) {
  const t = ev.type
  if (t === 'mouse') {
    const action = ev.action === 'move' ? 'moveTo' : (ev.action || 'click')
    const cmd = { type: 'mouse', action, space: 'view', button: ev.button || 'left' }
    if (['moveTo', 'click', 'down', 'up'].includes(action)) {
      // 脚本存逻辑坐标，转成物理客户区像素（× scale）
      cmd.x = Math.round((Number(ev.x) || 0) * appScale.value)
      cmd.y = Math.round((Number(ev.y) || 0) * appScale.value)
    }
    return cmd
  }
  if (t === 'delay') {
    return { type: 'delay', ms: Math.max(0, Math.min(60000, Math.round(Number(ev.ms) || 0))) }
  }
  if (t === 'input') {
    return { type: 'text', text: String(ev.text || '') }
  }
  if (t === 'key') {
    const base = String(ev.key || '').split('+').pop().trim()
    const vk = resolveVk(base)
    return vk ? { type: 'key', vk } : null
  }
  return null
}

function stamp() {
  const d = new Date()
  const p = (n, w = 2) => String(n).padStart(w, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`
}

function pushLog(level, msg) {
  logs.value.push({ time: stamp(), level, msg })
  if (logs.value.length > 500) logs.value.shift()
}

const running = ref(false)
let runToken = 0

async function onTryRun() {
  if (running.value) return
  const events = rawEvents.value
  if (!events.length) {
    pushLog('warn', '试跑：当前脚本没有步骤')
    return
  }
  running.value = true
  const token = ++runToken
  await onRefreshApp() // 确保拿到最新 scale/几何
  pushLog('info', `试跑开始，共 ${events.length} 步（scale=${appScale.value}）`)
  try {
    for (let i = 0; i < events.length; i++) {
      if (token !== runToken) {
        pushLog('info', '试跑已停止')
        break
      }
      const ev = events[i]
      if (ev.type === 'key' && String(ev.key || '').includes('+')) {
        pushLog('warn', `第 ${i + 1} 步：组合键 ${ev.key} 的修饰键暂不支持，仅发主键`)
      }
      const cmd = toCommand(ev)
      if (!cmd) {
        pushLog('warn', `第 ${i + 1} 步（${ev.type}）暂不支持，跳过`)
        continue
      }
      const timeout = cmd.type === 'delay' ? cmd.ms + 5000 : 10000
      try {
        await request(cmd, timeout)
        const conv =
          cmd.type === 'mouse' && cmd.x != null
            ? ` (${ev.x},${ev.y})→(${cmd.x},${cmd.y})`
            : ''
        pushLog(
          'info',
          `第 ${i + 1} 步 OK：${cmd.type}${cmd.action ? ':' + cmd.action : ''}${conv}`,
        )
      } catch (e) {
        pushLog('error', `第 ${i + 1} 步失败：${e.message}`)
        break
      }
    }
  } finally {
    if (token === runToken) {
      running.value = false
      pushLog('info', '试跑结束')
    }
  }
}

function onStop() {
  runToken++
  running.value = false
  pushLog('warn', '已请求停止')
}
// 用 rawEvents 重新渲染表格
function refreshRows() {
  rows.value = rawEvents.value.map(toRow)
}

function selectRow(i) {
  selectedRow.value = i
}

function requireScript() {
  if (!currentScript.value) {
    pushLog('warn', '先选择或新建一个脚本')
    return false
  }
  return true
}

// 新事件默认值
function defaultEvent(type) {
  if (type === 'delay') return { type: 'delay', ms: 500, remark: '' }
  if (type === 'findImage') {
    return { type: 'findImage', image: '', threshold: 0.9, timeout_ms: 3000, x: 0, y: 0, remark: '' }
  }
  if (type === 'input') return { type: 'input', text: '', remark: '' }
  if (type === 'key') return { type: 'key', key: 'Enter', action: 'press', remark: '' }
  return { type: 'mouse', action: 'click', button: 'left', x: 0, y: 0, remark: '' }
}

function openAdd(type) {
  if (!requireScript()) return
  dlgInitial.value = defaultEvent(type)
  dlgTitle.value = '添加事件'
  dlgIndex.value = -1
  dlgOpen.value = true
}

function openEdit(i) {
  const ev = rawEvents.value[i]
  if (!ev) return
  dlgInitial.value = { ...ev }
  dlgTitle.value = `修改第 ${i + 1} 步`
  dlgIndex.value = i
  dlgOpen.value = true
}

function onDialogSubmit(ev) {
  if (dlgIndex.value >= 0) {
    rawEvents.value.splice(dlgIndex.value, 1, ev)
    selectRow(dlgIndex.value)
    pushLog('info', `已修改第 ${dlgIndex.value + 1} 步`)
  } else {
    rawEvents.value.push(ev)
    selectRow(rawEvents.value.length - 1)
    pushLog('info', '已添加事件')
  }
  refreshRows()
}

function onAddMouse() { openAdd('mouse') }
function onAddDelay() { openAdd('delay') }
function onAddFindImage() { openAdd('findImage') }
function onAddInput() { openAdd('input') }
function onAddKey() { openAdd('key') }
function onMoveUp() {
  const i = selectedRow.value
  if (i <= 0) return
  const arr = rawEvents.value
  const tmp = arr[i - 1]
  arr[i - 1] = arr[i]
  arr[i] = tmp
  selectedRow.value = i - 1
  refreshRows()
}

function onMoveDown() {
  const i = selectedRow.value
  const arr = rawEvents.value
  if (i < 0 || i >= arr.length - 1) return
  const tmp = arr[i + 1]
  arr[i + 1] = arr[i]
  arr[i] = tmp
  selectedRow.value = i + 1
  refreshRows()
}
// 刷新应用：读回目标窗口几何（GET /api/app）
async function onRefreshApp() {
  try {
    const res = await fetch('/api/app')
    const j = await res.json()
    if (j && j.success && j.data) {
      const d = j.data
      const scale = Number(d.scale) || 1
      appScale.value = scale
      setScale(scale) // 取点面板的兜底 scale，与试跑换算同源
      const dpi = Math.round(scale * 100)
      appInfo.value = `${d.app || 'TTVoice.exe'} view ${d.w}x${d.h} DPI ${dpi}%`
    } else {
      console.warn('[app] 读取失败', j)
    }
  } catch (e) {
    console.warn('[app] 读取失败', e)
  }
}
function onClearLog() { logs.value = [] }

// 行内
async function onRowTryRun(i) {
  const ev = rawEvents.value[i]
  if (!ev) return
  await onRefreshApp()
  const cmd = toCommand(ev)
  if (!cmd) {
    pushLog('warn', `第 ${i + 1} 步（${ev.type}）暂不支持`)
    return
  }
  const timeout = cmd.type === 'delay' ? cmd.ms + 5000 : 10000
  try {
    await request(cmd, timeout)
    pushLog('info', `第 ${i + 1} 步 OK：${cmd.type}${cmd.action ? ':' + cmd.action : ''}`)
  } catch (e) {
    pushLog('error', `第 ${i + 1} 步失败：${e.message}`)
  }
}

function onRowEdit(i) {
  openEdit(i)
}

function onRowDelete(i) {
  if (!rawEvents.value[i]) return
  rawEvents.value.splice(i, 1)
  refreshRows()
  if (selectedRow.value >= rawEvents.value.length) {
    selectedRow.value = rawEvents.value.length - 1
  }
  pushLog('info', `已删除第 ${i + 1} 步`)
}
</script>

<template>
  <div class="editor" :class="'size-' + size">
    <!-- 顶部：脚本列表 + 控件尺寸 -->
    <div class="topbar">
      <span class="label">脚本列表</span>
      <select v-model="currentScript">
        <option v-for="s in scripts" :key="s" :value="s">{{ s }}</option>
      </select>
      <button @click="onRefreshScripts">刷新</button>
      <button @click="onNewScript">新建</button>
      <button @click="onSaveScript">保存</button>
      <button @click="onRenameScript">改名</button>
      <button @click="onDeleteScript">删除</button>

      <span class="spacer"></span>
      <span class="label">控件尺寸</span>
      <select v-model="size" class="size-select">
        <option value="small">小</option>
        <option value="medium">中（默认）</option>
        <option value="large">大</option>
        <option value="xlarge">超大</option>
      </select>
    </div>

    <!-- 工具栏 -->
    <div class="toolbar">
      <button @click="onTryRun" :disabled="running">试跑</button>
      <button @click="onStop">停止</button>
      <span class="divider"></span>
      <button @click="onAddMouse">添加鼠标</button>
      <button @click="onAddDelay">添加延迟</button>
      <button @click="onAddFindImage">添加找图</button>
      <button @click="onAddInput">添加输入</button>
      <button @click="onAddKey">添加按键</button>
      <span class="divider"></span>
      <button @click="onMoveUp">上移</button>
      <button @click="onMoveDown">下移</button>
      <span class="divider"></span>
      <button @click="onRefreshApp">刷新应用</button>
      <button @click="onClearLog">清日志</button>

      <span class="spacer"></span>
      <span class="app-info">{{ appInfo }}</span>
      <span class="case-count">共{{ rows.length }}行</span>
    </div>

    <!-- 脚本步骤表 -->
    <div class="table-wrap">
      <table>
        <colgroup>
          <col style="width: 56px" />
          <col style="width: 180px" />
          <col style="width: 72px" />
          <col style="width: 140px" />
          <col style="width: 110px" />
          <col style="width: 64px" />
          <col />
        </colgroup>
        <thead>
          <tr>
            <th>序号</th>
            <th>操作</th>
            <th>类型</th>
            <th>参数</th>
            <th>行为</th>
            <th>滚轮</th>
            <th>备注</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(row, i) in rows" :key="i" :class="{ selected: selectedRow === i }" @click="selectedRow = i">
            <td class="center">{{ i + 1 }}</td>
            <td class="ops">
              <button @click="onRowTryRun(i)">试跑</button>
              <button @click="onRowEdit(i)">修改</button>
              <button @click="onRowDelete(i)">删除行</button>
            </td>
            <td>{{ row.type }}</td>
            <td>{{ row.params }}</td>
            <td>{{ row.action }}</td>
            <td>{{ row.wheel }}</td>
            <td>{{ row.note }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- 日志 -->
    <div ref="logEl" class="logpanel" @scroll="onLogScroll">
      <div v-for="(l, i) in logs" :key="i" class="logline">
        <span class="time">{{ l.time }}</span>
        <span class="level" :class="'lv-' + l.level">{{ l.level }}:</span>
        <span class="msg">{{ l.msg }}</span>
      </div>
    </div>

    <!-- 状态栏 -->
    <div class="statusbar">
      <span class="status-item">
        <span class="dot" :class="{ on: status === '已连接' }"></span>{{ status }}
      </span>
      <span class="status-spacer"></span>
      <span class="status-item">脚本：{{ currentScript }}</span>
      <span class="sep">|</span>
      <span class="status-item">共 {{ rows.length }} 行</span>
      <span class="sep">|</span>
      <span class="status-item">控件尺寸：{{ sizeLabel }}</span>
      <span class="sep">|</span>
      <span class="status-item">{{ appInfo }}</span>
    </div>

    <EventDialog
      v-model:open="dlgOpen"
      :title="dlgTitle"
      :initial="dlgInitial"
      @submit="onDialogSubmit"
    />
  </div>
</template>

<style scoped>
/* ---- 全局控件尺寸：由 .size-* 上的 CSS 变量统一驱动 ---- */
.editor {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: #fff;
  color: #222;

  --font: 12px;
  --btn-h: 22px;
  --btn-pad: 0 8px;
  --row-pad: 3px 6px;
  --bar-pad: 6px 8px;
  --log-font: 12px;
  --log-h: 180px;
  --status-h: 24px;
}
.editor.size-small {
  --font: 11px;
  --btn-h: 18px;
  --btn-pad: 0 6px;
  --row-pad: 2px 5px;
  --bar-pad: 4px 6px;
  --log-font: 11px;
  --log-h: 150px;
  --status-h: 20px;
}
.editor.size-medium {
  --font: 12px;
  --btn-h: 22px;
  --btn-pad: 0 8px;
  --row-pad: 3px 6px;
  --bar-pad: 6px 8px;
  --log-font: 12px;
  --log-h: 180px;
  --status-h: 24px;
}
.editor.size-large {
  --font: 13px;
  --btn-h: 26px;
  --btn-pad: 0 10px;
  --row-pad: 5px 8px;
  --bar-pad: 8px 10px;
  --log-font: 13px;
  --log-h: 220px;
  --status-h: 28px;
}
.editor.size-xlarge {
  --font: 15px;
  --btn-h: 30px;
  --btn-pad: 0 12px;
  --row-pad: 7px 10px;
  --bar-pad: 10px 12px;
  --log-font: 15px;
  --log-h: 260px;
  --status-h: 32px;
}

/* 顶部 */
.topbar {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: var(--bar-pad);
  background: #ececec;
  border-bottom: 1px solid #c4c4c4;
}
.topbar .label {
  color: #333;
  font-size: var(--font);
}
.topbar select {
  min-width: 140px;
  height: var(--btn-h);
  font-size: var(--font);
}
.topbar select.size-select {
  min-width: 110px;
}

/* 按钮 */
button {
  height: var(--btn-h);
  padding: var(--btn-pad);
  font-size: var(--font);
  color: #222;
  background: #fbfbfb;
  border: 1px solid #b6b6b6;
  border-radius: 2px;
  cursor: pointer;
}
button:hover {
  background: #ededed;
}
button:active {
  background: #e2e2e2;
}

/* 工具栏 */
.toolbar {
  display: flex;
  align-items: center;
  gap: 4px;
  flex-wrap: wrap;
  padding: var(--bar-pad);
  background: #f6f6f6;
  border-bottom: 1px solid #d4d4d4;
}
.divider {
  width: 1px;
  height: calc(var(--btn-h) - 6px);
  margin: 0 4px;
  background: #cfcfcf;
}
.spacer {
  flex: 1;
}
.app-info,
.case-count {
  color: #555;
  font-size: var(--font);
}
.case-count {
  margin-left: 16px;
}

/* 表格 */
.table-wrap {
  flex: 1;
  overflow: auto;
}
table {
  width: 100%;
  border-collapse: collapse;
  table-layout: fixed;
}
th,
td {
  padding: var(--row-pad);
  font-size: var(--font);
  text-align: center;
  vertical-align: middle;
  white-space: normal; /* 长内容换行，不再截断 */
  word-break: break-word;
  border: 1px solid #cfcfcf;
}
th {
  font-weight: 600;
  background: #f4f4f4;
}
tbody tr:nth-child(even) {
  background: #fcfcfc;
}
tbody tr.selected {
  background: #e3f2fd;
}
td.center {
  text-align: center;
}
td.ops {
  padding: 2px 4px;
}
td.ops button {
  height: calc(var(--btn-h) - 2px);
  padding: 0 6px;
  margin-right: 2px;
  font-size: calc(var(--font) - 1px);
}

/* 日志 */
.logpanel {
  height: var(--log-h);
  padding: 6px 8px;
  overflow: auto;
  color: #ddd;
  background: #1e1e1e;
  border-top: 1px solid #000;
  font-family: Consolas, "Cascadia Mono", "Courier New", monospace;
  font-size: var(--log-font);
}
.logline {
  line-height: 1.6;
  white-space: pre;
}
.time {
  margin-right: 8px;
  color: #b8b8b8;
}
.level {
  margin-right: 6px;
}
.lv-debug {
  color: #b8b8b8;
}
.lv-info {
  color: #ffffff;
}
.lv-warn {
  color: #ffa726; /* 橙色 */
}
.lv-error {
  color: #ff5f56;
}

/* 状态栏 */
.statusbar {
  display: flex;
  align-items: center;
  flex: 0 0 auto;
  height: var(--status-h);
  padding: 0 10px;
  font-size: var(--font);
  color: #444;
  background: #ececec;
  border-top: 1px solid #c4c4c4;
}
.status-spacer {
  flex: 1;
}
.status-item {
  white-space: nowrap;
}
.sep {
  margin: 0 8px;
  color: #bbb;
}
.dot {
  display: inline-block;
  width: 8px;
  height: 8px;
  margin-right: 6px;
  border-radius: 50%;
  background: #c0392b;
}
.dot.on {
  background: #27ae60;
}
</style>
