<!-- 事件弹窗内的取坐标读数：实时显示鼠标在应用视图内的逻辑/物理坐标，
     按 F8 由宿主进程采样捕获后回填 X/Y。只读数据来自 /ws {"type":"cursor"}。 -->
<script setup>
import { computed } from 'vue'
import { currentPoint, cursor, fmtCoord } from './cursorStore'

// 优先显示冻结点，否则用窗口内的实时点
const point = computed(() => currentPoint())
const live = computed(() => cursor.sample)
const scale = computed(() => (point.value && point.value.scale) || cursor.scale || 1)
</script>

<template>
  <div class="readout" :class="{ dim: !point }">
    <div class="big">
      <span class="axis">逻辑</span>
      <span class="num">{{ point ? fmtCoord(point.lx) : '--' }}</span>
      <span class="comma">,</span>
      <span class="num">{{ point ? fmtCoord(point.ly) : '--' }}</span>
    </div>

    <div class="sub">
      物理 {{ live ? `${live.x}, ${live.y}` : '--' }} · 屏幕
      {{ live ? `${live.sx}, ${live.sy}` : '--' }}
    </div>
    <div class="sub">
      窗口 {{ live ? `${live.w}×${live.h}` : '--' }} @{{ fmtCoord(scale) }}
    </div>
  </div>
</template>

<style scoped>
.readout {
  margin: 2px 0 8px 58px;
  padding: 5px 8px 6px;
  background: #eef4ff;
  border: 1px solid #c9dbf7;
  border-radius: 3px;
  font-size: var(--font, 12px);
}
.readout.dim {
  background: #f3f3f3;
  border-color: #dcdcdc;
}
.big {
  display: flex;
  align-items: baseline;
  gap: 3px;
  font-family: Consolas, 'Cascadia Mono', monospace;
}
.big .axis {
  margin-right: 2px;
  color: #777;
  font-size: calc(var(--font, 12px) - 2px);
}
.big .num {
  font-size: calc(var(--font, 12px) + 6px);
  font-weight: 700;
  color: #14509c;
}
.big .comma {
  color: #999;
}
.sub {
  color: #555;
  font-family: Consolas, 'Cascadia Mono', monospace;
  font-size: calc(var(--font, 12px) - 1px);
  line-height: 1.5;
}
</style>
