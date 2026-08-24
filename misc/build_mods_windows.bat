@echo off
rem build_mods_windows.bat - build Lua C modules for Windows with MSVC.
rem Usage: build_mods_windows.bat [LUA_ROOT] [TS_ROOT]
rem   LUA_ROOT defaults to C:\Devel\Lua55
rem   TS_ROOT is optional; when given, also builds treesitter.dll.

setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "LUA_ROOT=%~1"
if "%LUA_ROOT%"=="" set "LUA_ROOT=C:\Devel\Lua55"
set "TS_ROOT=%~2"

rem Locate Visual Studio via vswhere and set up the x64 environment.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo ERROR: Visual Studio C++ tools not found
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "%LUA_ROOT%\include\lua.h" (
    echo ERROR: LUA_ROOT "%LUA_ROOT%" does not contain include\lua.h
    exit /b 1
)

set "LUA_LIB="
if exist "%LUA_ROOT%\lib\lua55.lib" set "LUA_LIB=%LUA_ROOT%\lib\lua55.lib"
if not defined LUA_LIB if exist "%LUA_ROOT%\lib\lua5.5.lib" set "LUA_LIB=%LUA_ROOT%\lib\lua5.5.lib"
if not defined LUA_LIB if exist "%LUA_ROOT%\lib\lua54.lib" set "LUA_LIB=%LUA_ROOT%\lib\lua54.lib"
if not defined LUA_LIB if exist "%LUA_ROOT%\lib\lua5.4.lib" set "LUA_LIB=%LUA_ROOT%\lib\lua5.4.lib"
if not defined LUA_LIB if exist "%LUA_ROOT%\lib\lua.lib" set "LUA_LIB=%LUA_ROOT%\lib\lua.lib"
if not defined LUA_LIB (
    echo ERROR: cannot find lua55.lib/lua5.5.lib/lua54.lib/lua5.4.lib/lua.lib under "%LUA_ROOT%\lib"
    exit /b 1
)

set "BASE_FLAGS=/nologo /W3 /D_CRT_SECURE_NO_DEPRECATE /MT /GS- /GL /Gy /Oy- /O2 /Oi /DNDEBUG /DLUA_BUILD_AS_DLL /I ."

echo === Building Lua modules with LUA_ROOT=%LUA_ROOT% ===

for %%m in (piecetab cellgrid termfeed json spantree) do (
    echo --- %%m ---
    cl %BASE_FLAGS% /I"%LUA_ROOT%\include" /I lua "%LUA_LIB%" /LD lua\%%m.c /Fe:lua\%%m.dll
    if errorlevel 1 exit /b 1
)

echo --- lua-utf8 ---
cl %BASE_FLAGS% /I"%LUA_ROOT%\include" /I lua "%LUA_LIB%" /LD lua\lutf8lib.c /Fe:lua\lua-utf8.dll
if errorlevel 1 exit /b 1

if not "%TS_ROOT%"=="" (
    if not exist "%TS_ROOT%\include\tree_sitter\api.h" (
        echo ERROR: TS_ROOT "%TS_ROOT%" does not contain include\tree_sitter\api.h
        exit /b 1
    )
    set "TS_LIB="
    if exist "%TS_ROOT%\lib\tree-sitter.lib" set "TS_LIB=%TS_ROOT%\lib\tree-sitter.lib"
    if not defined TS_LIB if exist "%TS_ROOT%\lib\tree-sitter.dll.lib" set "TS_LIB=%TS_ROOT%\lib\tree-sitter.dll.lib"
    if not defined TS_LIB (
        echo ERROR: cannot find tree-sitter.lib under "%TS_ROOT%\lib"
        exit /b 1
    )
    echo --- treesitter ---
    cl %BASE_FLAGS% /I"%LUA_ROOT%\include" /I lua /I"%TS_ROOT%\include" "%LUA_LIB%" /LD lua\treesitter.c "!TS_LIB!" /Fe:lua\treesitter.dll
    if errorlevel 1 exit /b 1
)

echo All Lua modules built successfully.
exit /b 0
