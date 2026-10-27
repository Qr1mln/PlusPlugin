import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    proxy: {
      // 开发期把 /api 转发到 framework.dll 的 CivetWeb(:47801)
      '/api': 'http://127.0.0.1:47801',
      // WebSocket 同样转发到 framework.dll 的 /ws
      '/ws': { target: 'ws://127.0.0.1:47801', ws: true },
    },
  },
})
