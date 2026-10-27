"""脚本编辑器启动入口：在仓库根目录跑 python -m editor.script_editor

实现在 editor/ 包（app / dialogs / engine_client / events / picker /
schedule / timepicker / target / win32）；本文件只保留启动入口。
"""
from editor.app import main

if __name__ == "__main__":
    main()
