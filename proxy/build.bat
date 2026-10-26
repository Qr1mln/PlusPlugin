@echo off
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

where ml64 >nul 2>nul
if errorlevel 1 (
    echo [ERROR] ml64.exe MASM assembler not found in PATH
    exit /b 1
)

if exist version.dll del /q version.dll
if exist stubs.obj del /q stubs.obj

ml64 /nologo /c /Fo stubs.obj stubs.asm
if errorlevel 1 (
    echo [ERROR] ml64 failed
    exit /b 1
)

rem /I"%~dp0.." puts the repo root on the include path so dllmain's
rem "framework/..." includes resolve (framework/ and sdk/ live at the repo root,
rem not under proxy/). Harmless if your VM already provides it another way.
cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 /std:c++17 /I"%~dp0.." *.cpp stubs.obj /link user32.lib /DEF:exports.def /OUT:version.dll /MACHINE:X64
if errorlevel 1 (
    echo [ERROR] link failed
    exit /b 1
)

echo.
echo [OK] built: %CD%\version.dll
dir /b version.dll
