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
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c /Fo:upload_lease_rpmalloc.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /I..\vendor\rpmalloc upload_lease_test.cpp upload_lease_rpmalloc.obj /Fe:upload_lease_report_test.exe /link kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
upload_lease_report_test.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DAOTR_PROD /I..\vendor\rpmalloc upload_lease_test.cpp upload_lease_rpmalloc.obj /Fe:upload_lease_prod_test.exe /link kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
upload_lease_prod_test.exe %*
exit /b %errorlevel%
