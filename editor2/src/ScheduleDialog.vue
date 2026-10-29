<script setup>
import { ref, reactive, computed, watch } from 'vue'
import {
  jobs,
  getSchedule,
  fetchScheduleRaw,
  saveSchedule,
  runScheduleNow,
  reloadSchedule,
} from './taskStore'
import { pauseTask, resumeTask, stopTask } from './taskStore'

const props = defineProps({
  open: { type: Boolean, default: false },
  tasks: { type: Array, default: () => [] },
})
const emit = defineEmits(['update:open'])

const TYPE_OPTIONS = [
  { value: 'daily', label: '每天/每周' },
  { value: 'interval', label: '固定间隔' },
  { value: 'once', label: '一次性' },
]
const DAY_LABELS = [
  { d: 1, t: '一' },
  { d: 2, t: '二' },
  { d: 3, t: '三' },
  { d: 4, t: '四' },
  { d: 5, t: '五' },
  { d: 6, t: '六' },
  { d: 7, t: '日' },
]
const MAX_SCRIPTS = 20

function nowStr() {
  const d = new Date()
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`
}
function genId() {
  return Math.random().toString(16).slice(2, 10)
}
function defaultJob() {
  return {
    id: genId(),
    name: '新任务',
    dir: '',
    scripts: [],
    enabled: true,
    loop: 1,
    gap_ms: 0,
    when: { type: 'daily', at: '21:30', days: [] },
  }
}

const sel = ref(-1)
const scriptPick = ref('')
const form = reactive({
  id: '',
  name: '',
  dir: '',
  scripts: [],
  typeKey: 'daily',
  at: '21:30',
  intervalValue: 30,
  intervalUnit: 'min',
  onceAt: nowStr(),
  days: [],
  loop: 1,
  gap_ms: 0,
  enabled: true,
})

// 一次性触发时间：拆成「日期 + 时刻」两个原生控件（不用 datetime-local）
const onceDateValue = computed({
  get: () => {
    const s = (form.onceAt || '').replace('T', ' ')
    return s.slice(0, 10) || nowStr().slice(0, 10)
  },
  set: (v) => {
    const s = (form.onceAt || '').replace('T', ' ')
    form.onceAt = `${v} ${s.slice(11, 16) || '00:00'}`
  },
})
const onceTimeValue = computed({
  get: () => {
    const s = (form.onceAt || '').replace('T', ' ')
    return s.slice(11, 16) || '00:00'
  },
  set: (v) => {
    const s = (form.onceAt || '').replace('T', ' ')
    form.onceAt = `${s.slice(0, 10) || nowStr().slice(0, 10)} ${v}`
  },
})

// 「运行中的任务」只显示还在跑/暂停的（已停止/完成的条目不再占这个区域）
const activeTasks = computed(() =>
  props.tasks.filter((t) => t.status === 'running' || t.status === 'paused'),
)


// 可用脚本名（GET /api/scripts），给脚本输入框做提示，避免手打错名字
const scriptNames = ref([])
async function loadScriptNames() {
  try {
    const res = await fetch('/api/scripts')
    const j = await res.json()
    if (j && j.success && Array.isArray(j.data)) scriptNames.value = j.data
  } catch (_) {
    // 宿主未连接：保留上次列表
  }
}

let pollTimer = null

// 列表展示：类型 / 状态 / 下次时间
function typeLabel(job) {
  const t = (job.when && job.when.type) || 'daily'
  if (t === 'interval') return '固定间隔'
  if (t === 'once') return '一次性'
  return '每天/每周'
}
function jobState(job) {
  if (job.running) return '运行中'
  if (job.pending) return '排队中'
  if (job.enabled === false) return '已停用'
  if (job.when && job.when.type === 'once' && !job.next_run) return '已结束'
  return ''
}
function nextText(job) {
  if (job.next && job.next !== '-') return job.next
  const w = job.when || {}
  if (w.type === 'once' && w.at) return w.at
  return '-'
}

function selectJob(i) {
  sel.value = i
  const job = jobs.value[i]
  if (!job) return
  form.id = job.id
  form.name = job.name || ''
  form.dir = job.dir || ''
  form.scripts = [...(job.scripts || [])]
  form.typeKey = (job.when && job.when.type) || 'daily'
  form.at = (job.when && job.when.at) || '21:30'
  // 间隔：后端新格式给 seconds，旧数据只有 minutes
  const secs = (job.when && (job.when.seconds || (job.when.minutes ? job.when.minutes * 60 : 0))) || 1800
  if (secs % 60 === 0) {
    form.intervalValue = secs / 60
    form.intervalUnit = 'min'
  } else {
    form.intervalValue = secs
    form.intervalUnit = 'sec'
  }
  form.onceAt = (job.when && job.when.at) || nowStr()
  form.days = [...((job.when && job.when.days) || [])]
  form.loop = job.loop || 1
  form.gap_ms = job.gap_ms || 0
  form.enabled = job.enabled !== false
}

function buildJob() {
  const when = { type: form.typeKey }
  if (form.typeKey === 'interval') {
    const base = Math.max(1, Number(form.intervalValue) || 1)
    when.seconds = form.intervalUnit === 'min' ? base * 60 : base
  } else if (form.typeKey === 'once') {
    when.at = (form.onceAt || '').replace('T', ' ')
  } else {
    when.at = form.at || '21:30'
    when.days = [...form.days]
  }
  return {
    id: form.id || genId(),
    name: form.name || '任务',
    dir: form.dir || '',
    scripts: [...form.scripts],
    enabled: form.enabled,
    loop: Number(form.loop) || 1,
    gap_ms: Number(form.gap_ms) || 0,
    when,
  }
}

function addScript() {
  const n = scriptPick.value
  if (!n) {
    hint.value = '先在下拉框里选一个脚本'
    return
  }
  if (form.scripts.length >= MAX_SCRIPTS) {
    hint.value = `一个任务最多 ${MAX_SCRIPTS} 个脚本`
    return
  }
  if (form.scripts.includes(n)) {
    hint.value = '该脚本已在序列中'
    scriptPick.value = ''
    return
  }
  form.scripts.push(n)
  scriptPick.value = ''
  hint.value = ''
}
function removeScript(i) {
  form.scripts.splice(i, 1)
}
function moveScript(i, d) {
  const t = i + d
  if (t < 0 || t >= form.scripts.length) return
  const arr = form.scripts
  ;[arr[i], arr[t]] = [arr[t], arr[i]]
}
function toggleDay(d) {
  const idx = form.days.indexOf(d)
  if (idx >= 0) form.days.splice(idx, 1)
  else form.days.push(d)
}

// 「启用/停用」按钮：只改当前表单的 enabled，保存后写入 schedule.json
function toggleEnabled() {
  form.enabled = !form.enabled
  hint.value = form.enabled
    ? '已设为启用（点「保存并生效」后参与自动触发）'
    : '已设为停用（不再自动触发，点「保存并生效」后生效）'
}

function applyEdit() {
  if (sel.value < 0) {
    hint.value = '先在左侧选中一个任务'
    return
  }
  const job = buildJob()
  const list = jobs.value.slice()
  list[sel.value] = job
  jobs.value = list
  hint.value = '已应用到列表（点「保存并生效」写入文件）'
}
function addJob() {
  const job = defaultJob()
  const list = jobs.value.slice()
  list.push(job)
  jobs.value = list
  selectJob(list.length - 1)
}
function deleteJob() {
  if (sel.value < 0) return
  const list = jobs.value.slice()
  list.splice(sel.value, 1)
  jobs.value = list
  sel.value = -1
}

async function onSave() {
  // 保存前先把右侧表单落到 jobs 上，否则改了表单却没点「应用修改」会写回旧数据。
  if (sel.value >= 0) applyEdit()
  if (jobs.value.length === 0) {
    hint.value = '没有任务可保存'
    return
  }
  const empty = jobs.value.find((j) => !j.scripts || j.scripts.length === 0)
  if (empty) {
    hint.value = `任务「${empty.name || '未命名'}」没有脚本，未保存`
    return
  }
  // 一次性任务的时间已经过去 -> 引擎不会再触发，保存后必须提醒，否则看起来像“定时任务不执行”
  const pastOnce = jobs.value.find((x) => {
    const w = x.when || {}
    if (w.type !== 'once' || !w.at) return false
    const t = new Date(String(w.at).replace(' ', 'T')).getTime()
    return Number.isFinite(t) && t <= Date.now()
  })
  const j = await saveSchedule(jobs.value)
  if (j && j.success) {
    hint.value = pastOnce
      ? `已保存，但「${pastOnce.name || '未命名'}」的一次性时间已过，不会触发`
      : '已保存并生效'
    if (sel.value >= 0 && jobs.value[sel.value]) selectJob(sel.value)
  } else {
    hint.value = '保存失败：检查脚本名是否存在、宿主是否在线'
  }
}
async function onRunNow() {
  if (sel.value < 0) {
    hint.value = '先在左侧选中一个任务'
    return
  }
  const id = jobs.value[sel.value].id
  const j = await runScheduleNow(id)
  hint.value = j && j.success ? '已提交立即执行' : '立即执行失败：任务不存在或启动失败'
}
async function onRefresh() {
  // 真正从 schedule.json 重新读盘（手工改过文件时用），再拉一遍状态
  const r = await reloadSchedule()
  if (!r || !r.success) {
    hint.value = '重载失败：宿主未连接'
    return
  }
  await getSchedule()
  if (sel.value >= 0 && jobs.value[sel.value]) selectJob(sel.value)
  hint.value = '已从文件重新加载'
}

const hint = ref('')

// 轮询只合并运行状态字段，不整体覆盖 jobs：避免正在编辑的任务被 3 秒轮询冲掉
async function pollStatus() {
  const list = await fetchScheduleRaw()
  if (!list) return
  for (const job of jobs.value) {
    const r = list.find((x) => x.id === job.id)
    if (!r) continue
    job.running = r.running
    job.pending = r.pending
    job.next = r.next
    job.next_run = r.next_run
  }
}

function openDialog() {
  sel.value = -1
  loadScriptNames()
  getSchedule().then(() => {
    if (jobs.value.length) selectJob(0)
  })
  if (pollTimer) clearInterval(pollTimer)
  pollTimer = setInterval(pollStatus, 3000)
}
function close() {
  if (pollTimer) clearInterval(pollTimer)
  pollTimer = null
  emit('update:open', false)
}

watch(
  () => props.open,
  (v) => {
    if (v) openDialog()
    else close()
  }
)

function statusLabel(s) {
  return (
    { running: '运行中', paused: '已暂停', done: '已完成', error: '错误', stopped: '已停止' }[s] || s
  )
}
function toLocalInput(s) {
  return s ? s.replace(' ', 'T') : ''
}
</script>

<template>
  <div v-if="props.open" class="mask" @mousedown.self="close">
    <div class="dlg">
      <div class="title">任务管理（定时任务）</div>

      <div class="body">
        <!-- 左：任务列表 -->
        <div class="list-pane">
          <div class="col-title">任务列表</div>
          <div v-if="jobs.length === 0" class="empty">（暂无任务，点底部「新增任务」）</div>
          <div
            v-for="(job, i) in jobs"
            :key="job.id"
            class="job-row"
            :class="{ active: sel === i }"
            @click="selectJob(i)"
          >
            <input
              type="checkbox"
              :checked="job.enabled !== false"
              @click.stop="job.enabled = !job.enabled"
            />
            <div class="job-main">
              <div class="job-name">
                {{ job.name || '(未命名)' }}<span v-if="jobState(job)" class="job-state"
                  >({{ jobState(job) }})</span
                >
              </div>
              <div class="job-sub">类型：{{ typeLabel(job) }}</div>
              <div class="job-next">下次：{{ nextText(job) }}</div>
            </div>
          </div>
        </div>

        <!-- 右：任务设置 -->
        <div class="form-pane">
          <div class="col-title">任务设置</div>
          <div class="field">
            <label>名称</label>
            <input v-model="form.name" class="inp" />
          </div>
          <div class="field">
            <label>脚本目录</label>
            <input v-model="form.dir" class="inp" placeholder="留空=scripts 默认；可填子目录" />
          </div>
          <div class="field">
            <label>脚本序列</label>
            <div class="script-add">
              <select v-model="scriptPick" class="inp">
                <option value="">
                  {{ scriptNames.length ? `（选择脚本，共 ${scriptNames.length} 个）` : '（列表为空，点 ↻ 刷新）' }}
                </option>
                <option v-for="n in scriptNames" :key="n" :value="n">{{ n }}</option>
              </select>
              <button class="mini" title="重新读取脚本列表" @click="loadScriptNames">↻</button>
              <button @click="addScript">添加</button>
            </div>
            <div class="script-list">
              <div v-for="(s, i) in form.scripts" :key="i" class="script-item">
                <span class="seq">{{ i + 1 }}.</span>
                <span class="sname">{{ s }}</span>
                <span class="sbtns">
                  <button @click="moveScript(i, -1)">↑</button>
                  <button @click="moveScript(i, 1)">↓</button>
                  <button @click="removeScript(i)">✕</button>
                </span>
              </div>
              <div v-if="form.scripts.length === 0" class="muted">（按列表顺序依次执行）</div>
            </div>
          </div>
          <div class="field">
            <label>触发</label>
            <select v-model="form.typeKey" class="inp">
              <option v-for="o in TYPE_OPTIONS" :key="o.value" :value="o.value">{{ o.label }}</option>
            </select>
          </div>

          <div v-if="form.typeKey === 'daily'" class="field sub">
            <label>每天/每周</label>
            <div class="daily-row">
              <input v-model="form.at" type="time" class="inp small" />
              <span class="muted">时刻</span>
              <span class="days">
                <label v-for="d in DAY_LABELS" :key="d.d" class="day">
                  <input type="checkbox" :checked="form.days.includes(d.d)" @click.stop="toggleDay(d.d)" />
                  {{ d.t }}
                </label>
              </span>
            </div>
            <div class="muted">（不勾 = 每天）</div>
          </div>

          <div v-else-if="form.typeKey === 'interval'" class="field sub">
            <label>固定间隔</label>
            <div class="daily-row">
              <input v-model.number="form.intervalValue" type="number" min="1" class="inp small" />
              <select v-model="form.intervalUnit" class="inp small">
                <option value="sec">秒</option>
                <option value="min">分钟</option>
              </select>
            </div>
          </div>

          <div v-else class="field sub">
            <label>一次性</label>
            <div class="once-row">
              <input v-model="onceDateValue" type="date" class="inp once-date" />
              <input v-model="onceTimeValue" type="time" class="inp once-time" />
            </div>
          </div>

          <div class="field">
            <label>轮数</label>
            <input v-model.number="form.loop" type="number" min="1" class="inp small" />
            <span class="muted" style="margin-left: 8px">脚本间隔</span>
            <input v-model.number="form.gap_ms" type="number" min="0" class="inp small" />
            <span class="muted">ms</span>
          </div>

          <div class="row-btns">
            <button @click="applyEdit">应用修改</button>
            <button @click="deleteJob">删除</button>
            <button
              class="en-btn"
              :class="form.enabled ? 'on' : 'off'"
              :title="form.enabled ? '当前已启用，点击停用' : '当前已停用，点击启用'"
              @click="toggleEnabled"
            >
              {{ form.enabled ? '停用' : '启用' }}
            </button>
          </div>
        </div>
      </div>

      <!-- 运行中的任务 -->
      <div class="running" v-if="activeTasks.length">
        <div class="col-title">运行中的任务</div>
        <div v-for="t in activeTasks" :key="t.id" class="run-row">
          <b>{{ t.name }}</b>
          <span class="task-status" :class="'st-' + t.status">{{ statusLabel(t.status) }}</span>
          <span class="muted">第{{ t.loop_cur }}/{{ t.loop_total }}轮 · 步{{ t.idx }}/{{ t.count }}</span>
          <span class="spacer"></span>
          <button v-if="t.status === 'running'" @click="pauseTask(t.id)">暂停</button>
          <button v-if="t.status === 'paused'" @click="resumeTask(t.id)">继续</button>
          <button @click="stopTask(t.id)">停止</button>
        </div>
      </div>

      <div class="foot">
        <button @click="addJob">新增任务</button>
        <button class="primary" @click="onSave">保存并生效</button>
        <button @click="onRunNow">立即执行</button>
        <button @click="onRefresh">刷新状态</button>
        <span class="hint">{{ hint }}</span>
        <span class="spacer"></span>
        <button @click="close">关闭</button>
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
  width: 900px;
  max-width: calc(100vw - 32px);
  max-height: calc(100vh - 32px);
  overflow: auto;
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
.body {
  display: flex;
  gap: 10px;
  align-items: stretch;
}
.col-title {
  font-weight: 600;
  margin-bottom: 6px;
  color: #333;
}
.list-pane {
  width: 300px;
  flex: 0 0 300px;
  border: 1px solid #c4d4ea;
  border-radius: 3px;
  background: #fff;
  padding: 8px;
  max-height: 460px;
  overflow: auto;
}
.empty {
  color: #888;
  font-size: calc(var(--font, 12px) - 1px);
}
.job-row {
  display: flex;
  gap: 6px;
  align-items: center;
  padding: 6px;
  border-radius: 3px;
  cursor: pointer;
}
.job-row:hover {
  background: #eef3fb;
}
.job-row.active {
  background: #dce8fb;
}
.job-main {
  flex: 1;
  min-width: 0;
}
.job-name {
  font-weight: 600;
}
.job-state {
  margin-left: 2px;
  font-weight: 400;
  color: #888;
}
.job-sub {
  color: #666;
  font-size: calc(var(--font, 12px) - 1px);
}
.job-next {
  margin-top: 2px;
  color: #2970c0;
  font-size: calc(var(--font, 12px) - 1px);
}
.form-pane {
  flex: 1;
  border: 1px solid #c4d4ea;
  border-radius: 3px;
  background: #fff;
  padding: 8px 10px;
}
.field {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 8px;
  flex-wrap: wrap;
}
.field > label {
  width: 64px;
  color: #444;
  flex: 0 0 64px;
}
.field.sub {
  background: #f4f7fc;
  border: 1px solid #e0e8f4;
  border-radius: 3px;
  padding: 6px 8px;
}
.inp {
  height: var(--btn-h, 22px);
  padding: 0 6px;
  font-size: var(--font, 12px);
  border: 1px solid #b6b6b6;
  border-radius: 2px;
  background: #fff;
  flex: 1;
  min-width: 80px;
}
.inp.small {
  flex: 0 0 80px;
}
.script-add {
  display: flex;
  gap: 4px;
  width: 100%;
}
.script-add .mini {
  flex: none;
  padding: 0 6px;
}
.script-list {
  width: 100%;
  margin-top: 4px;
  border: 1px solid #ddd;
  border-radius: 2px;
  max-height: 130px;
  overflow: auto;
}
.script-item {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 2px 6px;
  border-bottom: 1px solid #eee;
}
.script-item .seq {
  color: #888;
}
.script-item .sname {
  flex: 1;
}
.script-item .sbtns button {
  height: 18px;
  padding: 0 6px;
  margin-left: 2px;
}
.muted {
  color: #888;
  font-size: calc(var(--font, 12px) - 1px);
}
.daily-row {
  display: flex;
  align-items: center;
  gap: 6px;
}
.once-row {
  display: flex;
  align-items: center;
  gap: 6px;
  flex: 1;
}
.once-row .inp {
  flex: 0 0 auto;
  height: var(--btn-h, 22px);
  padding: 0 4px;
}
.once-row .once-date {
  width: 132px;
}
.once-row .once-time {
  width: 92px;
}
.days {
  display: inline-flex;
  gap: 4px;
  margin-left: 8px;
}
.day {
  display: inline-flex;
  align-items: center;
  gap: 1px;
}
.en-btn {
  margin-left: 4px;
}
.en-btn.on {
  color: #c0392b;
  border-color: #e2b7b1;
  background: #fdf3f2;
}
.en-btn.off {
  color: #1e8449;
  border-color: #b4d9c1;
  background: #f1faf4;
}
.row-btns {
  display: flex;
  gap: 6px;
  margin-top: 8px;
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
  background: #eef4ff;
  color: #14509c;
  font-weight: 600;
}
.running {
  margin-top: 10px;
  border: 1px solid #d8d8d8;
  border-radius: 3px;
  padding: 8px 10px;
  background: #fff;
}
.run-row {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 3px 0;
  flex-wrap: wrap;
}
.task-status {
  padding: 0 6px;
  border-radius: 2px;
  color: #fff;
  background: #888;
}
.task-status.st-running { background: #27ae60; }
.task-status.st-paused { background: #f39c12; }
.task-status.st-done { background: #2980b9; }
.task-status.st-error { background: #c0392b; }
.task-status.st-stopped { background: #888; }
.spacer {
  flex: 1;
}
.foot {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 12px;
}
.hint {
  color: #555;
}
</style>
