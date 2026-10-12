@echo off
rem ============================================================================
rem  build.bat -- Build TCP/IP Connection Monitor (CRT-based, ~111 KB)
rem
rem  Standard build using static CRT (/MT) for broad compatibility.
rem  For the smallest possible .exe (~17 KB), use build_nocrt.bat instead.
rem
rem  Usage:
rem    build.bat              Build Release x64 (default)
rem    build.bat x86          Build Release x86
rem    build.bat all          Build both x86 and x64 Release
rem    build.bat debug        Build Debug x64
rem    build.bat clean        Remove build outputs
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

if /i "%TARGET%"=="all" (
    call :build_arch x86
    echo.
    call :build_arch x64
    echo [DONE] Both builds complete.
    goto :eof
)

if /i "%TARGET%"=="x86"   call :build_arch x86   & goto :eof
if /i "%TARGET%"=="x64"   call :build_arch x64   & goto :eof
if /i "%TARGET%"=="debug" call :build_debug       & goto :eof

echo [ERROR] Unknown target: %TARGET%. Use x86, x64, all, debug, or clean.
exit /b 1

rem ---- Architecture build --------------------------------------------------
:build_arch
set "ARCH=%1"

echo ==========================================
echo  Building %ARCH% Release (CRT-based)
echo ==========================================

if /i "%ARCH%"=="x64" (call "%VCVARS64%" >nul 2>&1) else (call "%VCVARS32%" >nul 2>&1)

echo [1/2] Compiling resources and C source...
rc /nologo /fo tcplist.res tcplist.rc
if errorlevel 1 exit /b 1

cl /nologo /O1 /Os /MT /GS- /Gy /GL /W4 tcplist.c tcplist.res /Fe:tcplist.exe /link /OPT:REF /OPT:ICF /LTCG /MERGE:.rdata=.text /MERGE:.pdata=.text /SUBSYSTEM:WINDOWS kernel32.lib user32.lib gdi32.lib comctl32.lib iphlpapi.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

if /i "%ARCH%"=="x86" (if exist tcplist.exe move /Y tcplist.exe tcplist_x86.exe >nul)
if /i "%ARCH%"=="x64" (if exist tcplist.exe move /Y tcplist.exe tcplist_x64.exe >nul)

for %%F in (tcplist_%ARCH%.exe) do (
    set /a kb=%%~zF / 1024
    echo [SIZE] %%~nxF = %%~zF bytes (!kb! KB^)
)
echo [DONE]
goto :eof

rem ---- Debug build ---------------------------------------------------------
:build_debug
echo ==========================================
echo  Building x64 Debug
echo ==========================================
call "%VCVARS64%" >nul 2>&1

rc /nologo /fo tcplist.res tcplist.rc
if errorlevel 1 exit /b 1

cl /nologo /Zi /MTd /GS /Gy /W4 tcplist.c tcplist.res /Fe:tcplist_debug.exe /link /DEBUG:FULL /SUBSYSTEM:WINDOWS kernel32.lib user32.lib gdi32.lib comctl32.lib iphlpapi.lib advapi32.lib ws2_32.lib
if errorlevel 1 exit /b 1

for %%F in (tcplist_debug.exe) do (
    set /a kb=%%~zF / 1024
    echo [SIZE] %%~nxF = %%~zF bytes (!kb! KB^)
)
echo [DONE]
goto :eof
