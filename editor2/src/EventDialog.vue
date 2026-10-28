<script setup>
import { onBeforeUnmount, ref, watch } from 'vue'
import { cursor, start as startCursor, stop as stopCursor, beginPick, cancelPick } from './cursorStore'

const props = defineProps({
  open: { type: Boolean, default: false },
  title: { type: String, default: '事件' },
  initial: { type: Object, default: () => ({}) },
})
const emit = defineEmits(['update:open', 'submit'])

const TYPE_LABELS = { mouse: '鼠标', delay: '延迟', findImage: '找图', input: '输入', key: '按键' }
const MOUSE_ACTIONS = { click: '单击', move: '移动', down: '按下', up: '抬起' }
const MOUSE_BUTTONS = { left: '左键', right: '右键', middle: '中键' }
const KEY_ACTIONS = { press: '单击', down: '按下', up: '抬起' }

const form = ref({
  type: 'mouse',
  action: 'click',
  button: 'left',
  x: 0,
  y: 0,
  topmost: false,
  ms: 500,
  unit: '毫秒',
  image: '',
  threshold: 0.9,
  timeout_ms: 3000,
  text: '',
  key: 'Enter',
  keyAction: 'press',
  remark: '',
})

// "拾取时置顶"是个用一次就忘不掉的开关：存 localStorage，下次打开弹窗（含新建事件）
// 沿用上次的选择；事件自己有 topmost 字段时以事件为准。
const TOPMOST_KEY = 'pp.pickTopmost'

function loadPickTopmost() {
  try {
    return localStorage.getItem(TOPMOST_KEY) === '1'
  } catch (_) {
    return false
  }
}

function savePickTopmost(v) {
  try {
    localStorage.setItem(TOPMOST_KEY, v ? '1' : '0')
  } catch (_) {
    // 隐私模式下写不进去，忽略：只影响下次是否记住
  }
}

// 取坐标只在鼠标事件里需要：弹窗开着且类型为 mouse 时才轮询。
function isPickType(type) {
  return type === 'mouse'
}

function syncPolling() {
  if (props.open && isPickType(form.value.type)) startCursor()
  else stopCursor()
}

watch(
  () => props.open,
  (open) => {
    if (!open) {
      stopCursor()
      return
    }
    const ev = props.initial || {}
    const ms = Number(ev.ms ?? 500) || 0
    const useSeconds = ms >= 1000 && ms % 1000 === 0
    form.value = {
      type: ev.type || 'mouse',
      action: ev.action || 'click',
      button: ev.button || 'left',
      x: ev.x ?? 0,
      y: ev.y ?? 0,
      topmost: ev.topmost === undefined ? loadPickTopmost() : ev.topmost === true,
      ms: useSeconds ? ms / 1000 : ms,
      unit: useSeconds ? '秒' : '毫秒',
      image: ev.image || '',
      threshold: ev.threshold ?? 0.9,
      timeout_ms: ev.timeout_ms ?? 3000,
      text: ev.text || '',
      key: ev.key || 'Enter',
      keyAction: ev.type === 'key' ? ev.action || 'press' : 'press',
      remark: ev.remark || '',
    }
    syncPolling()
  },
  { immediate: true },
)

// 切换事件类型时跟着开/关轮询（延迟、按键不需要坐标）
watch(() => form.value.type, syncPolling)

function num(v) {
  const n = Number(v) || 0
  return Number.isInteger(n) ? n : Math.round(n * 10) / 10
}

// 解析 "123,566" / "123，566" / "(123, 566)" 这类坐标对
function parseXY(text) {
  const m = String(text || '').match(
    /^\s*\(?\s*(-?\d+(?:\.\d+)?)\s*[,，]\s*(-?\d+(?:\.\d+)?)\s*\)?\s*$/,
  )
  return m ? [Number(m[1]), Number(m[2])] : null
}

// 粘贴到 X 或 Y 任一格：若是坐标对，则同时填入 X 和 Y
function onPasteXY(e) {
  const text = (e.clipboardData || window.clipboardData)?.getData('text') || ''
  const pair = parseXY(text)
  if (pair) {
    e.preventDefault()
    form.value.x = pair[0]
    form.value.y = pair[1]
  }
}

// 弹窗开着时按 F8 捕获到新点，直接回填 X/Y
watch(
  () => cursor.frozen,
  (f) => {
    if (!f || !props.open || !isPickType(form.value.type)) return
    form.value.x = Math.round(Number(f.lx) * 10) / 10
    form.value.y = Math.round(Number(f.ly) * 10) / 10
  },
)

// 拾取模式返回的点，同样回填 X/Y
watch(
  () => cursor.picked,
  (p) => {
    if (!p || !props.open || !isPickType(form.value.type)) return
    form.value.x = Math.round(Number(p.lx) * 10) / 10
    form.value.y = Math.round(Number(p.ly) * 10) / 10
  },
)

function onPick() {
  if (cursor.picking) cancelPick()
  else beginPick({ topmost: form.value.topmost === true })
}

function close() {
  stopCursor()
  emit('update:open', false)
}

onBeforeUnmount(stopCursor)

function submit() {
  const f = form.value
  let ev
  if (f.type === 'mouse') {
    ev = {
      type: 'mouse',
      action: f.action,
      button: f.button,
      x: num(f.x),
      y: num(f.y),
      topmost: f.topmost === true,
      remark: f.remark.trim(),
    }
  } else if (f.type === 'delay') {
    const ms = Math.round(Number(f.ms || 0) * (f.unit === '秒' ? 1000 : 1))
    ev = { type: 'delay', ms: Math.max(0, Math.min(60000, ms)), remark: f.remark.trim() }
  } else if (f.type === 'findImage') {
    ev = {
      type: 'findImage',
      image: f.image.trim(),
      threshold: Number(f.threshold) || 0.9,
      timeout_ms: Math.round(Number(f.timeout_ms) || 0),
      x: num(f.x),
      y: num(f.y),
      remark: f.remark.trim(),
    }
  } else if (f.type === 'input') {
    ev = { type: 'input', text: f.text, remark: f.remark.trim() }
  } else {
    ev = {
      type: 'key',
      key: f.key.trim() || 'Enter',
      action: f.keyAction,
      remark: f.remark.trim(),
    }
  }
  emit('submit', ev)
  close()
}

function onKeydown(e) {
  if (e.key === 'Escape') {
    close()
  } else if (e.key === 'Enter' && !e.shiftKey && form.value.type !== 'input') {
    e.preventDefault()
    submit()
  }
}
</script>

<template>
  <div v-if="open" class="mask" @mousedown.self="close">
    <div class="dlg" @keydown="onKeydown">
      <div class="title">{{ title }}</div>

      <div class="row">
        <label>类型</label>
        <select v-model="form.type">
          <option v-for="(label, key) in TYPE_LABELS" :key="key" :value="key">{{ label }}</option>
        </select>
      </div>

      <template v-if="form.type === 'mouse'">
        <div class="row">
          <label>动作</label>
          <select v-model="form.action">
            <option v-for="(l, k) in MOUSE_ACTIONS" :key="k" :value="k">{{ l }}</option>
          </select>
        </div>
        <div class="row">
          <label>按键</label>
          <select v-model="form.button">
            <option v-for="(l, k) in MOUSE_BUTTONS" :key="k" :value="k">{{ l }}</option>
          </select>
        </div>
        <div class="row">
          <label>X / Y</label>
          <input
            v-model="form.x"
            type="text"
            inputmode="decimal"
            class="small"
            placeholder="x"
            @paste="onPasteXY"
          />
          <input
            v-model="form.y"
            type="text"
            inputmode="decimal"
            class="small"
            placeholder="y"
            @paste="onPasteXY"
          />
          <button type="button" class="pick" @click="onPick">
            {{ cursor.picking ? '拾取中…（点一下）' : '左键拾取' }}
          </button>
        </div>
        <div class="row">
          <label>窗口</label>
          <label class="check">
            <input
              v-model="form.topmost"
              type="checkbox"
              @change="savePickTopmost(form.topmost)"
            />
            拾取时置顶（把应用窗口提到最前再取点）
          </label>
        </div>
      </template>

      <template v-else-if="form.type === 'delay'">
        <div class="row">
          <label>时长</label>
          <input v-model="form.ms" type="number" class="small" />
          <select v-model="form.unit">
            <option>毫秒</option>
            <option>秒</option>
          </select>
        </div>
      </template>

      <template v-else-if="form.type === 'findImage'">
        <div class="row">
          <label>图片</label>
          <input v-model="form.image" class="wide" placeholder="模板图片路径" />
        </div>
        <div class="row">
          <label>阈值</label>
          <input v-model="form.threshold" type="number" step="0.01" class="small" />
        </div>
        <div class="row">
          <label>超时</label>
          <input v-model="form.timeout_ms" type="number" class="small" /> ms
        </div>
      </template>

      <template v-else-if="form.type === 'input'">
        <div class="row">
          <label>文本</label>
          <textarea v-model="form.text" rows="4" class="wide"></textarea>
        </div>
      </template>

      <template v-else>
        <div class="row">
          <label>按键</label>
          <input v-model="form.key" class="wide" placeholder="Enter / F1 / Ctrl+A" />
        </div>
        <div class="row">
          <label>动作</label>
          <select v-model="form.keyAction">
            <option v-for="(l, k) in KEY_ACTIONS" :key="k" :value="k">{{ l }}</option>
          </select>
        </div>
      </template>

      <div class="row">
        <label>备注</label>
        <input v-model="form.remark" class="wide" />
      </div>

      <div class="foot">
        <span class="hint">Enter 确定　Esc 取消</span>
        <button class="primary" @click="submit">确定</button>
        <button @click="close">取消</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.mask {
  position: fixed;
  inset: 0;
  z-index: 50;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.35);
}
.dlg {
  width: 420px;
  max-width: calc(100vw - 32px);
  padding: 12px 14px;
  background: #f7f7f7;
  border: 1px solid #b8b8b8;
  border-radius: 4px;
  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.28);
  font-size: var(--font, 12px);
  color: #222;
}
.title {
  margin-bottom: 10px;
  font-weight: 600;
  font-size: calc(var(--font, 12px) + 1px);
}
.row {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 6px;
}
.row > label {
  flex: 0 0 52px;
  color: #333;
}
.row input,
.row select,
.row textarea {
  height: var(--btn-h, 22px);
  padding: 0 6px;
  font-size: var(--font, 12px);
  border: 1px solid #b6b6b6;
  border-radius: 2px;
  background: #fff;
}
.row textarea {
  height: auto;
  padding: 4px 6px;
  resize: vertical;
}
.small {
  width: 90px;
}
.row > label.check {
  flex: 1;
  display: flex;
  align-items: center;
  gap: 4px;
  color: #333;
  cursor: pointer;
}
.row > label.check input {
  height: auto;
  margin: 0;
  padding: 0;
}
.pick {
  height: var(--btn-h, 22px);
  padding: 0 8px;
  font-size: calc(var(--font, 12px) - 1px);
  border: 1px solid #3a7afe;
  border-radius: 2px;
  background: #eef4ff;
  color: #14509c;
  cursor: pointer;
  white-space: nowrap;
}
.wide {
  flex: 1;
  min-width: 0;
}
.foot {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-top: 12px;
}
.hint {
  flex: 1;
  color: #888;
  font-size: calc(var(--font, 12px) - 1px);
}
button {
  height: var(--btn-h, 22px);
  padding: 0 12px;
  font-size: var(--font, 12px);
  border: 1px solid #b6b6b6;
  border-radius: 2px;
  background: #fbfbfb;
  cursor: pointer;
}
button:hover {
  background: #ededed;
}
button.primary {
  border-color: #3a7afe;
  background: #3a7afe;
  color: #fff;
}
button.primary:hover {
  background: #2f6ae6;
}
</style>
