@echo off
setlocal
if defined AOTR_VCVARS32 (
    call "%AOTR_VCVARS32%"
    if errorlevel 1 exit /b 1
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
    if errorlevel 1 exit /b 1
)
where cl >nul 2>&1
if errorlevel 1 (
    echo Use an x86 Native Tools prompt or set AOTR_VCVARS32.
    exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /DAOTR_PROD /LD /I..\vendor\rpmalloc /Fe:bfme2_accel.new.dll aotr_accel.cpp rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib /MAP
exit /b %errorlevel%
