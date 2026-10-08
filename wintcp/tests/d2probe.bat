@echo off
REM d2probe.bat - build and run the SIO_TCP_INFO stall probe.
REM
REM WHY THIS EXISTS. D2 was "one socket whose SIO_TCP_INFO does not return
REM blocks every socket behind it". Deciding what to do about that needed a
REM number, and the number is machine-specific. Measured on the host that
REM produced the design, by walking every socket one at a time:
REM
REM     of 391 sockets walked:  4 answered SIO_TCP_INFO
REM                             387 failed at once (WSAENOTSOCK / WSAENOTCONN)
REM                             1 call took 28,281 ms   (a tailscaled socket)
REM                             the next was still running after 90 s
REM
REM Those numbers are why kProbeWorkers is 16 and not 4, and why kNoProgressMs
REM is 250 and not 4000. On a different machine they will be different, and
REM the first thing to do about that is re-run this.
REM
REM WHAT IT IS NOT. Not a test, and not part of any gate. It walks the system
REM handle table and issues one ioctl per socket on its own thread, which is
REM deliberately the wrong shape for CI: slow, and it depends on what the
REM machine happens to be doing. It reports; cli.bat and selftest decide.
REM
REM EXPECT IT TO LOOK HUNG. The default run walks sockets SEQUENTIALLY - that
REM ordering is the whole point, since it is the only way to attribute a stall
REM to one socket - and the first socket whose ioctl does not return stops the
REM walk dead until the budget expires. On the machine that produced the
REM numbers above, that meant an apparent hang. It is the defect being
REM measured, not a bug in the probe. Ctrl+C is safe: nothing is written
REM outside temp\.
REM
REM For a fast check that the probe still builds and that the product's
REM handle-table fetch is sound, use the --productpath mode, which makes no
REM ioctl calls at all:
REM     d2probe.bat --productpath
REM
REM USAGE (run from anywhere: every path below is resolved from this script's
REM own directory, so ..\..\ is the repository root).
REM     wintcp\tests\d2probe.bat              build, then probe, 60 s budget
REM     wintcp\tests\d2probe.bat 20000        probe with a 20 s per-pass budget
REM     wintcp\tests\d2probe.bat --productpath fast self-check, no stall risk
REM
REM The probe reads its PIDs of interest from <repo>\temp\d2pids.txt, so that
REM file must exist or it will report zero sockets - which looks like a clean
REM result and measures nothing. Generate it from the repository root:
REM     build\wintcp.exe list --columns pid --format csv ^
REM         | findstr /r "^[0-9]" > temp\d2pids.txt

setlocal EnableExtensions

REM Scratch space and the probe binary live in the repository's temp\, so this
REM writes nothing into wintcp\tests\ and never depends on the caller's cwd.
set ROOT=%~dp0..\..

set VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VS%" set VS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VS%" set VS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VS%" (
    echo D2PROBE: no vcvars64.bat found - edit the VS variable at the top.
    exit /b 1
)

REM mstcpip.h hides TCP_INFO_v0 below NTDDI_WIN10_RS2 and the project builds
REM with _WIN32_WINNT=0x0601, so this one translation unit overrides the
REM target the same way SocketTraffic.cpp does.
if not exist "%ROOT%\temp" mkdir "%ROOT%\temp"
call "%VS%" >nul 2>&1
cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE ^
   /D_WIN32_WINNT=0x0601 /DWIN32_LEAN_AND_MEAN ^
   /Fo"%ROOT%\temp\d2probe.obj" /Fe"%ROOT%\temp\d2probe.exe" ^
   "%~dp0d2probe.cpp" /link ws2_32.lib
if errorlevel 1 (
    echo D2PROBE: build failed.
    exit /b 1
)

echo.
echo D2PROBE: built. Probing - this issues one SIO_TCP_INFO per socket, so it
echo          takes as long as the machine's slowest socket takes.
echo.
if not exist "%ROOT%\temp\d2pids.txt" (
    echo D2PROBE: %ROOT%\temp\d2pids.txt is missing - see the header for how
    echo          to generate it, or the probe will report zero sockets.
    echo.
)
"%ROOT%\temp\d2probe.exe" %1
endlocal
exit /b 0