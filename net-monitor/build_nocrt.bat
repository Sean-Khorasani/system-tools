@echo off
rem ============================================================================
rem  build_nocrt.bat -- NO-CRT minimal build (~17 KB) for TCP/IP Connection Monitor
rem
rem  Produces the absolute smallest possible self-contained .exe using:
rem    - Assembly entry point (startup_x64.asm / startup_x86.asm)
rem    - Assembly memcpy + memset (memcpy_x64.asm / memset_x64.asm)
rem    - /NODEFAULTLIB -- zero CRT overhead
rem    - /OPT:REF /OPT:ICF /LTCG -- dead-code elimination
rem
rem  Usage:
rem    build_nocrt.bat          Build Release x64 (default)
rem    build_nocrt.bat x86      Build Release x86
rem    build_nocrt.bat clean    Remove build artifacts
rem ============================================================================

setlocal enabledelayedexpansion

rem ---- Detect Visual Studio 2022 ------------------------------------------
set "VCVARS64="
set "VCVARS32="
for %%E in (Enterprise Professional Community Preview) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        if "!VCVARS64!"=="" set "VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if "!VCVARS32!"=="" set "VCVARS32=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars32.bat"
    )
)
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    if "%VCVARS64%"=="" set "VCVARS64=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    if "%VCVARS32%"=="" set "VCVARS32=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
)
if "%VCVARS64%"=="" (
    echo [ERROR] Visual Studio 2022 not found.
    echo Install VS 2022 Community - free - or Build Tools from:
    echo   https://visualstudio.microsoft.com/downloads/
    exit /b 1
)

rem ---- Parse target -------------------------------------------------------
set "TARGET=%1"
if "%TARGET%"=="" set "TARGET=x64"

if /i "%TARGET%"=="clean" (
    del /q *.obj *.res *.exe *.pdb 2>nul
    echo [CLEAN] Done.
    goto :eof
)

if /i "%TARGET%"=="x64" goto :build_x64
if /i "%TARGET%"=="x86" goto :build_x86
echo [ERROR] Unknown target: %TARGET%. Use x86, x64, or clean.
exit /b 1

rem =========================================================================
rem  X64 NO-CRT BUILD
rem =========================================================================
:build_x64
echo ==========================================
echo  Building NO-CRT x64 (~17 KB target)
echo ==========================================
call "%VCVARS64%" >nul 2>&1

rem Compile resources
echo [1/4] Compiling resources...
rc /nologo /fo tcplist.res tcplist.rc
if errorlevel 1 exit /b 1

rem Assemble startup + memory functions
echo [2/4] Assembling startup and memory functions...
ml64 /c /nologo /Fo startup.obj startup_x64.asm
if errorlevel 1 exit /b 1
ml64 /c /nologo /Fo memset.obj  memset_x64.asm
if errorlevel 1 exit /b 1
ml64 /c /nologo /Fo memcpy.obj  memcpy_x64.asm
if errorlevel 1 exit /b 1

rem Compile C
echo [3/4] Compiling C source...
cl /nologo /O1 /Os /Oi /GS- /Gy /GL /W4 /DNO_CRT /c /Fo tcplist.obj tcplist.c
if errorlevel 1 exit /b 1

rem Link
echo [4/4] Linking...
link /nologo /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG /MERGE:.rdata=.text /MERGE:.pdata=.text tcplist.obj startup.obj memset.obj memcpy.obj tcplist.res /OUT:tcplist.exe kernel32.lib user32.lib gdi32.lib comctl32.lib iphlpapi.lib advapi32.lib
if errorlevel 1 exit /b 1

goto :report

rem =========================================================================
rem  X86 NO-CRT BUILD
rem =========================================================================
:build_x86
echo ==========================================
echo  Building NO-CRT x86 target
echo ==========================================
call "%VCVARS32%" >nul 2>&1

echo [1/4] Compiling resources...
rc /nologo /fo tcplist.res tcplist.rc
if errorlevel 1 exit /b 1

echo [2/4] Assembling startup...
ml /c /nologo /Fo startup.obj startup_x86.asm
if errorlevel 1 exit /b 1

echo [3/4] Compiling C source...
cl /nologo /O1 /Os /Oi /GS- /Gy /GL /W4 /DNO_CRT /c /Fo tcplist.obj tcplist.c
if errorlevel 1 exit /b 1

rem For x86, provide memcpy/memset via minimal CRT libs
echo [4/4] Linking...
link /nologo /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG /MERGE:.rdata=.text /MERGE:.pdata=.text tcplist.obj startup.obj tcplist.res /OUT:tcplist_x86.exe kernel32.lib user32.lib gdi32.lib comctl32.lib iphlpapi.lib advapi32.lib libvcruntime.lib libcmt.lib
if errorlevel 1 exit /b 1

goto :report

:report
for %%F in (tcplist*.exe) do (
    set /a kb=%%~zF / 1024
    echo [SIZE] %%~nxF = %%~zF bytes (!kb! KB^)
)
echo [DONE] Build complete.
goto :eof
