@echo off
rem step 1 - read-only status: CC/USER/GRBM + active CU count
rem expected stock: CC=0xFF000000, active CUs = 8
cd /d "%~dp0"
ravencuu.exe status
echo.
pause
