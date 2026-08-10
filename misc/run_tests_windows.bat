@echo off
rem run_tests_windows.bat — build & run all C unit tests with MSVC.
rem Locates Visual Studio via vswhere, sets up the x64 environment,
rem generates test entries with lua, compiles each test with cl and runs it.
rem Backslash paths for cmd; gen_entries.lua normalizes them itself.

setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo ERROR: Visual Studio C++ tools not found
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

set "TESTS=tests\linecache_test_fanout4 tests\linecache_test_fanout8 tests\piecetab_test_fanout4 tests\undotree_test tests\cellgrid_test tests\termfeed_test"

for %%t in (%TESTS%) do (
    echo === %%t ===
    lua misc\gen_entries.lua %%t.c %%t.gen.inc win32
    if errorlevel 1 exit /b 1
    cl /nologo /W4 /WX /std:c11 /D_CRT_SECURE_NO_WARNINGS /utf-8 /I. /Itests /Fe%%t.exe /Fo%%t.obj %%t.c
    if errorlevel 1 exit /b 1
    %%t.exe
    if errorlevel 1 exit /b 1
    del %%t.obj >nul 2>&1
)
echo All Windows tests passed!
