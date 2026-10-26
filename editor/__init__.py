r"""pplugin 脚本编辑器（编辑器 UI + 推送客户端）。

模块划分：
  win32           Win32(ctypes) 绑定、常量、结构体与窗口小工具
  events          事件模型：类型/标签常量、键名解析、事件清洗与展示
  engine_client   script-engine.dll 的 TCP 帧协议客户端（EngineClient / EngineSession）
  target          宿主应用窗口查找（Target / find_target）
  picker          坐标拾取（全局鼠标钩子 + 半透明遮罩）
  dialogs         事件编辑对话框（EventDialog）
  app             主窗口（App）与 main() 入口
  script_editor   启动入口（python editor/script_editor.py）
"""