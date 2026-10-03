@echo off
setlocal
rem Run from an x86 Native Tools prompt, or set AOTR_VCVARS32 to vcvars32.bat.
if defined AOTR_VCVARS32 (
    call "%AOTR_VCVARS32%"
    if errorlevel 1 exit /b 1
)
where cl >nul 2>&1
if errorlevel 1 (
    echo Use an x86 Native Tools prompt or set AOTR_VCVARS32.
    exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c /Fo:buffer_tracking_rpmalloc.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /I..\vendor\rpmalloc buffer_tracking_test.cpp buffer_tracking_rpmalloc.obj /Fe:buffer_tracking_report_test.exe /link kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
buffer_tracking_report_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DAOTR_PROD /I..\vendor\rpmalloc buffer_tracking_test.cpp buffer_tracking_rpmalloc.obj /Fe:buffer_tracking_prod_test.exe /link kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
buffer_tracking_prod_test.exe
exit /b %errorlevel%
