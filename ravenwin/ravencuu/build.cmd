@echo off
rem Builds ravencuu.exe with the VS C++ toolchain, no CRT:
rem  - links only kernel32 + shell32; all CRT pieces live in ravencuu.c
rem  - /ENTRY:main (console entry point receives argc/argv from the kernel)
rem  - /GS- no stack cookies, /Zl no default CRT libraries
rem Big buffers are file-static in ravencuu.c so no __chkstk is emitted.
rem /MANIFESTUAC makes the exe request elevation via UAC on its own;
rem the boot task (SYSTEM) and elevated shells are unaffected.
setlocal
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo VS C++ toolchain not found - install "Desktop development with C++" Build Tools
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O1 /Os /GS- /Zl /utf-8 /W3 ravencuu.c /Fe:ravencuu.exe /link /NODEFAULTLIB kernel32.lib shell32.lib user32.lib gdi32.lib /ENTRY:WinMain /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MERGE:.rdata=.text /FILEALIGN:512 /MANIFEST:EMBED "/MANIFESTUAC:"level='requireAdministrator'""
