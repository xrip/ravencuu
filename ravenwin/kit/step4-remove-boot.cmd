@echo off
rem step 4 - remove the boot task (next boot = stock 8 CU).
rem Run this BEFORE step5 cleanup if you want the drivers gone too.
cd /d "%~dp0"
ravencuu.exe uninstall-autostart
echo.
pause
