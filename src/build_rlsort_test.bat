@echo off
setlocal
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
cl /nologo /O2 /MT /W3 /EHsc /Fe:rlsort_test.exe rlsort_test.cpp /link kernel32.lib /BASE:0x70000000 /FIXED /DYNAMICBASE:NO
exit /b %errorlevel%
