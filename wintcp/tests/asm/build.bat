@echo off
REM SPDX-License-Identifier: Apache-2.0
REM Build script for the assembly optimization test project.
REM
REM Usage: build.bat [Debug|Release]
REM
REM Prerequisites:
REM   - Visual Studio 2022 with C++ build tools (x64)
REM   - Run from "Developer Command Prompt for VS 2022" or
REM     call vcvarsall.bat before running this script

setlocal enabledelayedexpansion

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set SRC_DIR=%~dp0
set BUILD_DIR=%SRC_DIR%build

echo === Building testAssemblies (%CONFIG%) ===
echo Source: %SRC_DIR%
echo Build:  %BUILD_DIR%

REM Try cmake first, then fall back to direct cl invocation
where cmake >nul 2>&1
if %ERRORLEVEL%==0 (
    echo --- Using CMake ---
    cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
    if %ERRORLEVEL% neq 0 (
        echo CMake configuration failed.
        exit /b 1
    )
    cmake --build "%BUILD_DIR%" --config %CONFIG%
    if %ERRORLEVEL% neq 0 (
        echo Build failed.
        exit /b 1
    )
    echo.
    echo === Running tests ===
    "%BUILD_DIR%\bin\%CONFIG%\testAssemblies.exe"
    exit /b %ERRORLEVEL%
)

REM Fallback: direct compilation with cl.exe
echo --- Using cl.exe directly ---

set COMMON_FLAGS=/std:c++17 /EHsc /utf-8 /permissive- /Zc:__cplusplus /W4 /MT
set RELEASE_FLAGS=/O2 /Gy /GL /GF /Os
set DEBUG_FLAGS=/Od /Zi /MTd

if /i "%CONFIG%"=="Debug" (
    set OPT_FLAGS=%DEBUG_FLAGS%
) else (
    set OPT_FLAGS=%RELEASE_FLAGS%
)

if not exist "%BUILD_DIR%\obj" mkdir "%BUILD_DIR%\obj"

cl %COMMON_FLAGS% %OPT_FLAGS% ^
    /I "%SRC_DIR%\..\..\src" ^
    /DWIN32_LEAN_AND_MEAN /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0601 ^
    /Fe:"%BUILD_DIR%\testAssemblies.exe" ^
    /Fo:"%BUILD_DIR%\obj\\" ^
    "%SRC_DIR%testAssemblies.cpp" ^
    "%SRC_DIR%opt_functions.cpp" ^
    /link /MANIFEST:NO iphlpapi.lib ws2_32.lib comctl32.lib user32.lib

if %ERRORLEVEL% neq 0 (
    echo Compilation failed.
    exit /b 1
)

echo.
echo === Running tests ===
"%BUILD_DIR%\testAssemblies.exe"
exit /b %ERRORLEVEL%
