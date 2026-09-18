@echo off
rem step 5 - remove the BYOVD services and System32 driver copies
rem (remove the boot task first - see step4)
cd /d "%~dp0"
ravencuu.exe cleanup
echo.
pause
