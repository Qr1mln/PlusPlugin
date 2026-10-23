@echo off
rem ===========================================================================
rem build_plugin.bat - builds empty.dll and drops it into the plugins root.
rem Used to verify the M0 load/unload path end to end.
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

if exist empty.dll del /q empty.dll

cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 empty_plugin.cpp /link /OUT:empty.dll /MACHINE:X64
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

set "DEST=%APPDATA%\TTVoice\plugins\000-empty"
if not exist "%DEST%" mkdir "%DEST%"
copy /y empty.dll "%DEST%\" >nul
copy /y plugin.json "%DEST%\" >nul

echo.
echo [OK] installed: %DEST%
dir /b "%DEST%"
