@echo off
setlocal
if defined AOTR_VCVARS32 (
    call "%AOTR_VCVARS32%"
    if errorlevel 1 exit /b 1
)
where cl >nul 2>&1
if errorlevel 1 exit /b 1
if "%~1"=="" exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c /Fo:rotwk_skeleton_rpmalloc.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /I..\vendor\rpmalloc rotwk_skeleton_test.cpp rotwk_skeleton_rpmalloc.obj /Fe:rotwk_skeleton_report_test.exe /link /BASE:0x18000000 /FIXED kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
rotwk_skeleton_report_test.exe "%~1"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /DAOTR_PROD /I..\vendor\rpmalloc rotwk_skeleton_test.cpp rotwk_skeleton_rpmalloc.obj /Fe:rotwk_skeleton_prod_test.exe /link /BASE:0x18000000 /FIXED kernel32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
rotwk_skeleton_prod_test.exe "%~1" %2
exit /b %errorlevel%
