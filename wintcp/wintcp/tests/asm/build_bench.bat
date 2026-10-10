@echo off
REM build_bench.bat
REM SPDX-License-Identifier: Apache-2.0
REM
REM Build the two benchmark executables and the optimized static library
REM for comparing wintcp's hand-written C++ hot paths against their
REM assembly/intrinsic-optimized replacements.
REM
REM Produces (in build_bench\):
REM   wintcp_asm_opt.lib  - optimized implementations (from opt_functions.cpp)
REM   bench_orig.exe      - wintcp_benchmark.cpp + orig_functions.cpp
REM   bench_asm.exe       - wintcp_benchmark.cpp + replaced_functions.cpp
REM                         + asm_functions.cpp + wintcp_asm_opt.lib
REM
REM Usage:
REM   build_bench.bat           Build everything (default)
REM   build_bench.bat clean     Remove build_bench\ first, then build
REM   build_bench.bat lib       Build only wintcp_asm_opt.lib
REM   build_bench.bat orig      Build only bench_orig.exe
REM   build_bench.bat asm       Build only bench_asm.exe
REM
REM Requirements: "Developer Command Prompt for VS 2022" (or x64 Native Tools
REM Command Prompt).  If cl.exe is not on PATH the script will try to locate
REM vcvars64.bat automatically.

setlocal EnableExtensions EnableDelayedExpansion

set "BENCH_DIR=%~dp0"
set "BENCH_DIR=%BENCH_DIR:~0,-1%"
set "ASM_DIR=%BENCH_DIR%"
set "BENCH_SRC=%ASM_DIR%\bench"
set "OUT_DIR=%ASM_DIR%\build_bench"

set "CL_FLAGS=/std:c++17 /EHsc /W4 /permissive- /Zc:__cplusplus /utf-8 /DUNICODE /D_UNICODE /O2 /MT /guard:cf /c"
set "INC_FLAGS=/I %BENCH_SRC% /I %ASM_DIR% /I %ASM_DIR%\..\..\src"
set "LINK_FLAGS=/SUBSYSTEM:CONSOLE /MANIFEST:NO /guard:cf Advapi32.lib Ws2_32.lib"

set "DO_CLEAN=0"
set "BUILD_LIB=1"
set "BUILD_ORIG=1"
set "BUILD_ASM=1"

if /I "%~1"=="clean" set "DO_CLEAN=1"
if /I "%~1"=="lib"  ( set "BUILD_ORIG=0" & set "BUILD_ASM=0" )
if /I "%~1"=="orig" ( set "BUILD_LIB=0" & set "BUILD_ASM=0" )
if /I "%~1"=="asm"  ( set "BUILD_LIB=0" & set "BUILD_ORIG=0" )

if "%DO_CLEAN%"=="1" (
    echo Cleaning %OUT_DIR% ...
    if exist "%OUT_DIR%" rmdir /S /Q "%OUT_DIR%"
)

REM -- Locate MSVC if cl.exe is not on PATH ----------------------------------
where cl.exe >nul 2>&1
if not errorlevel 1 goto :have_cl

echo cl.exe not found on PATH. Trying to set up MSVC via vcvars64.bat...
call :try_vcvars "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call :try_vcvars "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
call :try_vcvars "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
call :try_vcvars "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call :try_vcvars "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:have_cl
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cl.exe not found.
    echo Open "Developer Command Prompt for VS 2022" and run this script.
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set "FAILED=0"

REM === 1. opt_functions.cpp -> opt_functions.obj -> wintcp_asm_opt.lib ===
if "%BUILD_LIB%"=="1" (
    echo [1/4] Compiling opt_functions.cpp ...
    cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\opt_functions.obj" "%ASM_DIR%\opt_functions.cpp" || (
        echo ERROR: failed to compile opt_functions.cpp
        set "FAILED=1"
    )
    if not "!FAILED!"=="1" (
        echo [2/4] Creating wintcp_asm_opt.lib ...
        lib /nologo /out:"%OUT_DIR%\wintcp_asm_opt.lib" "%OUT_DIR%\opt_functions.obj" || (
            echo ERROR: failed to create wintcp_asm_opt.lib
            set "FAILED=1"
        )
    )
)

REM === 2. bench_orig.exe = harness + orig_functions ===
REM NOTE: no parentheses in echo text below; a ")" inside a
REM parenthesized block ends the block early in cmd.exe.
if "%BUILD_ORIG%"=="1" if not "!FAILED!"=="1" (
    echo [3/4] Compiling wintcp_benchmark.cpp + orig_functions.cpp ...
    cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\wb_orig.obj" "%ASM_DIR%\wintcp_benchmark.cpp"
    if errorlevel 1 (
        echo ERROR: failed to compile wintcp_benchmark.cpp for orig
        set "FAILED=1"
    )
    if not "!FAILED!"=="1" (
        cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\orig_functions.obj" "%BENCH_SRC%\orig_functions.cpp"
        if errorlevel 1 (
            echo ERROR: failed to compile orig_functions.cpp
            set "FAILED=1"
        )
    )
    if not "!FAILED!"=="1" (
        echo Linking bench_orig.exe ...
        link %LINK_FLAGS% /OUT:"%OUT_DIR%\bench_orig.exe" "%OUT_DIR%\wb_orig.obj" "%OUT_DIR%\orig_functions.obj"
        if errorlevel 1 (
            echo ERROR: failed to link bench_orig.exe
            set "FAILED=1"
        )
    )
)

REM === 3. bench_asm.exe = harness + replaced + asm_functions + opt_lib ===
if "%BUILD_ASM%"=="1" if not "!FAILED!"=="1" (
    echo [4/4] Compiling wintcp_benchmark.cpp + replaced + asm functions ...
    cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\wb_asm.obj" "%ASM_DIR%\wintcp_benchmark.cpp"
    if errorlevel 1 (
        echo ERROR: failed to compile wintcp_benchmark.cpp for asm
        set "FAILED=1"
    )
    if not "!FAILED!"=="1" (
        cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\replaced_functions.obj" "%BENCH_SRC%\replaced_functions.cpp"
        if errorlevel 1 (
            echo ERROR: failed to compile replaced_functions.cpp
            set "FAILED=1"
        )
    )
    if not "!FAILED!"=="1" (
        cl %CL_FLAGS% %INC_FLAGS% /Fo"%OUT_DIR%\asm_functions.obj" "%BENCH_SRC%\asm_functions.cpp"
        if errorlevel 1 (
            echo ERROR: failed to compile asm_functions.cpp
            set "FAILED=1"
        )
    )
    if not "!FAILED!"=="1" (
        echo Linking bench_asm.exe ...
        link %LINK_FLAGS% /OUT:"%OUT_DIR%\bench_asm.exe" "%OUT_DIR%\wb_asm.obj" "%OUT_DIR%\replaced_functions.obj" "%OUT_DIR%\asm_functions.obj" "%OUT_DIR%\wintcp_asm_opt.lib"
        if errorlevel 1 (
            echo ERROR: failed to link bench_asm.exe
            set "FAILED=1"
        )
    )
)

if not "%BUILD_LIB%"=="1" if not "%BUILD_ORIG%"=="1" if not "%BUILD_ASM%"=="1" (
    echo Nothing to build.
    exit /b 0
)

if "!FAILED!"=="1" (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

echo.
echo Build succeeded:
echo   %OUT_DIR%\wintcp_asm_opt.lib
echo   %OUT_DIR%\bench_orig.exe
echo   %OUT_DIR%\bench_asm.exe
echo.
echo Run the report:
echo   python run_benchmark_report.py
exit /b 0

:try_vcvars
if not exist "%~1" exit /b 0
echo Calling: %~1
call "%~1" >nul 2>&1
exit /b 0
