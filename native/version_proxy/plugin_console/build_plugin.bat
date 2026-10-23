@echo off
rem ===========================================================================
rem build_plugin.bat - builds the log console plugin and installs it.
rem Installed as 010-console so it loads after the empty sample.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Install Visual Studio Build Tools first.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [ERROR] No Visual Studio with MSVC x64 toolset found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

if exist log_console.dll del /q log_console.dll

cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 log_console.cpp ^
   /link /OUT:log_console.dll /MACHINE:X64 user32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

set "DEST=%APPDATA%\TTVoice\plugins\010-console"
if not exist "%DEST%" mkdir "%DEST%"
copy /y log_console.dll "%DEST%\" >nul
copy /y plugin.json "%DEST%\" >nul

echo.
echo [OK] installed: %DEST%
dir /b "%DEST%"
