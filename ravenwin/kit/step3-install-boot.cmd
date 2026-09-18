@echo off
rem step 3 - register the boot task "ravencuu":
rem at every startup (SYSTEM, highest) ravencuu runs the pounce sequence
rem with 3 retries. Brief display blip before login is expected.
rem The task binds to THIS copy of ravencuu.exe (the one you are running),
rem wherever it lives - so run it from a boot-mounted disk (C:\RavenCU),
rem not from a USB stick.
cd /d "%~dp0"
ravencuu.exe install-autostart
echo.
pause
