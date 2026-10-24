@echo off
rem ===========================================================================
rem build_plugin.bat - builds the script plugin (runs on the VM, see build_plugin.py)
rem Output is script.dll; install into plugins\script is done by build_plugin.py
rem (it has to do the enabled=false / enabled=true dance for hot reload).
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

if exist script.dll del /q script.dll

rem /I"%~dp0.."  -> plugin/ so "../sdk/pplugin.h" resolves to the repo sdk/
rem /I"%~dp0..\.." -> repo root, keeps room for other shared includes
rem The engine include itself is relative to the source file.
cl /nologo /LD /MT /O2 /EHsc /W4 /utf-8 /I"%~dp0.." /I"%~dp0..\.." script_plugin.cpp ^
   /link /OUT:script.dll /MACHINE:X64 user32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo.
echo [OK] built: %CD%\script.dll
dir /b script.dll
