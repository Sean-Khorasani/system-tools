@echo off
REM readmegolden.bat - prove every command printed in README.md still runs.
REM
REM WHY IT EXISTS. golden.bat pins the product's behaviour against hand-written
REM cases. It does NOT pin the README: nothing stopped a case study from
REM showing a switch that was renamed, a flag that was removed, or a grammar
REM that changed, and the first person to find out would be a reader following
REM the documentation. This closes that gap by executing the commands the
REM documentation actually shows.
REM
REM WHY POWERSHELL AND NOT PURE CMD. A batch `for /f` loop cannot carry a
REM nested double quote, and the README legitimately contains one - the
REM documented way to keep a space inside a filter value is
REM   --filter "note:""corporate dns"""
REM which a batch loop truncates to `--filter "note:""corporate`, silently
REM turning a valid command into a rejected one. A false FAILURE is bad, but a
REM harness that has to be special-cased for its own input file is worse.
REM PowerShell's argument parser handles the quoting the way a reader's shell
REM does, which is precisely the property being tested.
REM
REM A CORRECTNESS NOTE, because getting this wrong is instructive. The first
REM version of this harness parsed the exit code inside a `for` block with
REM `!RC!` but WITHOUT `EnableDelayedExpansion`, so `!RC!` was never
REM substituted, the comparison against "2" could never be true, and the script
REM reported "0 rejected" for every command - including `list --pid 1`, which
REM is a known rc-2 rejection. It printed a clean pass while measuring
REM nothing, and the pass was quoted in a session write-up as evidence. The
REM two defences against that class of bug are both here: delayed expansion is
REM enabled explicitly at the top, and the SELF-TEST below proves the harness
REM DETECTS a rejection rather than merely asserting that it does.
REM
REM WHAT IS ASSERTED, and what deliberately is not:
REM   * rc 2 (bad arguments) is a FAILURE. That is the only failure mode this
REM     harness can see, and it is the one that matters: it means the README
REM     shows a command the binary rejects.
REM   * rc 0 and rc 1 are both fine. rc 1 is "nothing matched", which is a
REM     legitimate answer for a filter on a machine that has no such row - the
REM     studies are examples, not fixtures, and asserting on live row counts
REM     would make the gate fail every time the machine changed.
REM   * Output content is NOT asserted. A study's example output is captured
REM     on one machine and is explicitly documented as machine-specific; a
REM     harness that pinned it would break on every refresh. The README says so
REM     too ("Output varies per machine; the PIDs, process names, ports and byte
REM     totals below are from one Windows 11 host").
REM   * Mutating verbs run in --dry-run / non-destructive form only. The
REM     commands file is checked-in data and this runs unattended; nothing here
REM     may kill a process or write a firewall rule. The `bookmark` and
REM     `preset` verbs DO round-trip against HKCU with a unique name and clean
REM     up after themselves, exactly as golden.bat already does.
REM
REM Exit codes: 0 all documented commands accepted, 1 at least one rejected.
REM
REM Usage: readmegolden.bat [path-to-exe] [commands-file]
REM        (default: build\wintcp.exe, readme_cmds.txt)

setlocal EnableExtensions EnableDelayedExpansion
set BIN=%~1
if "%BIN%"=="" set BIN=build\wintcp.exe
if not exist "%BIN%" (
    echo READMEGOLDEN: missing %BIN% - run build.bat first.
    exit /b 1
)
set CMDFILE=%~2
if "%CMDFILE%"=="" set CMDFILE=readme_cmds.txt
if not exist "%CMDFILE%" (
    echo READMEGOLDEN: missing %CMDFILE% - the list of documented commands.
    exit /b 1
)

set BAD=0
set N=0
set OUT=%TEMP%\wnreadme_out.txt

REM ---- SELF-TEST: delegated to the PowerShell driver ---------------------------
REM A gate that has never been seen to fail is not a gate. The most dangerous
REM failure mode for a checker like this is one that reports success no matter
REM what it is given - and the FIRST version of this script did exactly that
REM (see the header). The driver therefore runs two commands with KNOWN exit
REM codes before the real list, and refuses to report a number unless the
REM verdicts are what they must be. That check lives in readmegolden.ps1
REM because it needs the same argument handling as the real run; all this
REM wrapper does is surface its verdict.
set PSOUT=%TEMP%\wnreadme_ps.txt
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0readmegolden.ps1" -Bin "%BIN%" -CmdFile "%CMDFILE%" > "%PSOUT%" 2>&1
type "%PSOUT%"
findstr /c:"READMEGOLDEN: self-test ok" "%PSOUT%" >nul 2>&1
if errorlevel 1 (
    echo.
    echo READMEGOLDEN: ABORTED - the driver's self-test did not confirm that it
    echo            can detect a known rejection. It is not measuring what it
    echo            claims to, so no verdict is reported. See the output above.
    del "%PSOUT%" >nul 2>&1
    exit /b 1
)
for /f "tokens=2,3" %%A in ('findstr /c:"COUNTS:" "%PSOUT%"') do (
    set N=%%A
    set BAD=%%B
)
del "%PSOUT%" >nul 2>&1
echo.
echo READMEGOLDEN: %N% documented commands, %BAD% rejected.
if "%BAD%"=="0" (
    echo READMEGOLDEN: PASS - every command shown in the README is accepted.
    exit /b 0
)
echo READMEGOLDEN: FAIL - the README shows a command the binary rejects.
exit /b 1
