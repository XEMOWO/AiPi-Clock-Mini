@echo off
REM WB2 Serial Tool - build script (run on Windows)
REM Usage: double-click or run "build.bat" in cmd
REM Requires: Windows Python 3.10+ (PySide6>=6.8 for Python 3.13)

echo === Installing dependencies ===
python -m pip install -r requirements.txt

echo === Building with PyInstaller ===
pyinstaller --onefile --windowed --name WB2SerialTool ^
    --collect-all qfluentwidgets ^
    --exclude-module PySide6.QtWebEngineCore ^
    --exclude-module PySide6.QtWebEngineWidgets ^
    --exclude-module PySide6.Qt3DCore ^
    main.py

echo === Done ===
echo Output: dist\WB2SerialTool.exe
pause
