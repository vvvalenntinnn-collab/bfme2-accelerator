@echo off
setlocal
if defined AOTR_VCVARS32 (
    call "%AOTR_VCVARS32%"
    if errorlevel 1 exit /b 1
)
where cl >nul 2>&1
if errorlevel 1 exit /b 1
if "%~1"=="" (
    echo Usage: build_rotwk_offload_test.bat temporary-reference.bin [benchmark]
    exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /EHsc rotwk_offload_test.cpp /Fe:rotwk_offload_test.exe /link /BASE:0x18000000 /FIXED kernel32.lib
if errorlevel 1 exit /b 1
rotwk_offload_test.exe "%~1" %2
exit /b %errorlevel%
