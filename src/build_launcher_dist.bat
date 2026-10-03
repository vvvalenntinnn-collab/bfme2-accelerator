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
echo === building bfme2_accel_loader.exe (32-bit GUI, embedded cover art, elevated manifest) ===
if not exist ..\dist mkdir ..\dist
call "%~dp0prepare_art.bat"
rc /nologo /fo launcher.res launcher.rc
if errorlevel 1 ( echo RESOURCE BUILD FAILED & exit /b 1 )
cl /nologo /O2 /MT /W3 /Fe:..\dist\bfme2_accel_loader.exe launcher.cpp launcher.res /link kernel32.lib user32.lib advapi32.lib gdi32.lib gdiplus.lib ole32.lib comdlg32.lib /SUBSYSTEM:WINDOWS /MANIFEST:NO
if errorlevel 1 ( echo LOADER BUILD FAILED & exit /b 2 )
copy /y ..\packaging\README.txt ..\dist\README.txt >nul
