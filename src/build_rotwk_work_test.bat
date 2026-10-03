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
if "%~1"=="" (
    echo Usage: build_rotwk_work_test.bat temporary-reference.bin [benchmark]
    exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c /Fo:rotwk_work_rpmalloc.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /I..\vendor\rpmalloc rotwk_work_test.cpp rotwk_work_rpmalloc.obj /Fe:rotwk_work_report_test.exe /link /BASE:0x18000000 /FIXED kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
rotwk_work_report_test.exe "%~1"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DAOTR_PROD /I..\vendor\rpmalloc rotwk_work_test.cpp rotwk_work_rpmalloc.obj /Fe:rotwk_work_prod_test.exe /link /BASE:0x18000000 /FIXED kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
rotwk_work_prod_test.exe "%~1" %2
exit /b %errorlevel%
