@echo off
chcp 65001 >nul
cd /d %~dp0
echo ════════════════════════════════════════
echo   WB2-clock2 一键烧录 (Windows 原生版)
echo ════════════════════════════════════════
echo.
echo 1. 确认板子已用 USB 线连好电脑
echo 2. 设备管理器 - 端口(COM和LPT) 里能看到串口
echo    (看不到就装 CH340 驱动后重新插拔)
echo.
set /p PORT=请输入串口号 (直接回车默认 COM3):
if "%PORT%"=="" set PORT=COM3
echo.
echo 现在请操作板子:
echo   按住 BOOT 键 - 按一下 RST 键 - 松开 BOOT
pause
echo.
echo 正在烧录到 %PORT% ...
%~dp0bflb_iot_tool.exe --chipname=BL602 --baudrate=921600 --port=%PORT% --pt=%~dp0partition_cfg_2M.toml --dts=%~dp0bl_factory_params_IoTKitA_40M.dts --firmware=%~dp0WB2-clock2.bin
echo.
echo 烧录结束, 按任意键退出
pause
