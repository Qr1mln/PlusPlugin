# editor2（Vue 3 + Vite）

脚本编辑器前端。**当前只实现 UI；所有操作（刷新/新建/保存/改名/删除、试跑/停止、添加鼠标/延迟/找图/输入/按键、上移/下移、刷新应用、清日志、行内试跑/修改/删除）均为占位，未接实际逻辑。**

## 开发

```bash
cd editor2
npm install
npm run dev
```

默认地址 http://localhost:5173

## 构建

```bash
npm run build      # 产物在 editor2/dist/
npm run preview
```

## 目录

```
editor2/
  index.html          Vite 入口
  vite.config.js
  src/
    main.js
    App.vue           编辑器界面
    style.css
```
