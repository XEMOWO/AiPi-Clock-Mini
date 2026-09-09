#!/usr/bin/env python3
"""
WB2 串口工具 - Windows EXE
开发: python main.py
打包: build.bat
"""
import sys
import os

# 保证 app 包可导入(冻结时也在 exe 目录内)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PySide6.QtWidgets import QApplication
from app.main_window import MainWindow


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("WB2 串口工具")
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
