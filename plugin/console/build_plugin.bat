@echo off
rem ===========================================================================
rem build_plugin.bat - builds the console plugin and installs it.
rem Output is console.dll, installed into plugins\console.
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

if exist console.dll del /q console.dll

rem /I"%~dp0.." puts plugin/ on the include path so the source's
rem "../sdk/ttplugin.h" resolves to the repo-root sdk/ directory.
cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 /I"%~dp0.." log_console.cpp ^
   /link /OUT:console.dll /MACHINE:X64 d2d1.lib dwrite.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

set "DEST=%APPDATA%\TTVoice\plugins\console"
if not exist "%DEST%" mkdir "%DEST%"
copy /y console.dll "%DEST%\" >nul
copy /y plugin.json "%DEST%\" >nul

echo.
echo [OK] installed: %DEST%
dir /b "%DEST%"
