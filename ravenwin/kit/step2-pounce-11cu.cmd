@echo off
rem step 2 - the unlock: disable -> write -> raced enable.
rem Display may freeze/go black - EXPECTED. A mid-init wedge is possible;
rem hard reset recovers. Results are in ravencuu.log either way.
cd /d "%~dp0"
ravencuu.exe pounce --count 11 --confirm --retries 3
echo.
pause
