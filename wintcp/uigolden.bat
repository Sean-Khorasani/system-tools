@echo off
REM uigolden.bat - the GUI half of the golden suite, SPLIT OUT of golden.bat.
REM
REM WHY IT IS A SEPARATE FILE. golden.bat's own contract is "Fast, no desktop,
REM CI-safe" - it must run on a build agent with no interactive session. The
REM one thing in it that could not honour that was the trailing `--uiharness`
REM smoke: that flag deliberately falls through to MainWindow::Create, drives
REM the real WndProc, and needs a window (and a desktop, and a message pump).
REM Keeping it inside golden meant every "headless" run still popped a window,
REM and a CI agent with no session either failed it or had to pass a magic
REM `quick` argument to opt out - a flag whose only purpose was to skip the
REM GUI, which is a smell that the split was overdue.
REM
REM So: golden.bat is now CLI-only, unconditionally headless, with no opt-out
REM flag to remember. This file owns everything that needs a window.
REM
REM WHAT IS HERE, AND WHAT IS NOT. This is the AUTOMATED GUI surface only: the
REM in-process behavioural harness (`--uiharness`), which drives the real
REM window's WndProc. It is NOT a human test, and it cannot be: it asserts
REM state through the same entry points a user drives, but nobody LOOKED at a
REM pixel. The things only a person can judge - is the theme right, does the
REM list scroll smoothly, does the tray balloon appear, does the window look
REM right on a second monitor - are listed at the bottom as a manual checklist
REM and are deliberately NOT automated. A green run here means "the window
REM behaves", not "the window looks right".
REM
REM Exit codes: 0 pass, 1 fail. The harness is FATAL in this file, where it was
REM advisory in golden.bat - the split is the whole point, so a GUI regression
REM now fails something.
REM
REM Usage: uigolden.bat [path-to-exe]   (default: build\wintcp.exe)

setlocal EnableExtensions
set BIN=%~1
REM The GUI harness moved OUT of the product on 2026-10-02: wintcp.exe no
REM longer has --uiharness. Default to the test driver, which creates its
REM own window. A caller may still pass the product path explicitly.
if "%BIN%"=="" set BIN=build\tests\wintcp-tests.exe
if not exist "%BIN%" (
    echo UIGOLDEN: missing %BIN% - run build.bat first.
    exit /b 1
)

set HOUT=%TEMP%\wnuigolden_uih_%RANDOM%.txt
if exist "%HOUT%" del "%HOUT%"

echo.
echo UIGOLDEN: running wintcp-tests.exe ui ^(needs a desktop session^)...
"%BIN%" ui > "%HOUT%" 2>&1
set RC=%ERRORLEVEL%

REM Parse the verdict out of the captured output rather than trusting rc alone.
REM The harness prints "=== UI HARNESS: N passed, M failed ===="; matching that
REM line is what distinguishes "the harness ran and reported" from "the harness
REM crashed before reporting", which are different failures and used to look
REM the same to whoever read the tail.
REM
REM Token positions, which the first version got wrong: the line splits to
REM   [0]==  [1]UI  [2]HARNESS:  [3]N  [4]passed,  [5]M  [6]failed  [7]====
REM so N is token 4 and M is token 6 in 1-based `for /f` terms. A fixed `tokens=3,4,5`
REM picked up "HARNESS:" as the count and printed an empty failure number, which
REM reported FAIL on a clean 32/0 run - a harness that fails when the thing it
REM tests passes is worse than no harness, so this is worth spelling out.
set PASSED=
set FAILED=
for /f "tokens=4,6" %%A in ('findstr /c:"UI HARNESS:" "%HOUT%"') do (
    set PASSED=%%A
    set FAILED=%%B
)

echo.
echo UIGOLDEN: harness output
echo ---------------------------------------------
type "%HOUT%"
echo ---------------------------------------------
echo.

if not defined PASSED (
    echo UIGOLDEN: FAIL - the harness produced no verdict line ^(rc=%RC%^).
    echo           It most likely could not create a window. That is expected
    echo           on a headless build agent, and is the reason this lives in
    echo           its own file: run it on a machine with a desktop session.
    if exist "%HOUT%" del "%HOUT%"
    exit /b 1
)

REM rc is a second opinion, not the verdict: the harness's own count is
REM authoritative and is what a reader wants to see.
if not "%FAILED%"=="0" (
    echo UIGOLDEN: FAIL - %FAILED% harness check^(s^) failed of %PASSED% checked ^(rc=%RC%^).
    if exist "%HOUT%" del "%HOUT%"
    exit /b 1
)
if not "%RC%"=="0" (
    echo UIGOLDEN: FAIL - zero harness failures but the process exited %RC%.
    echo           A non-zero exit with a clean count is itself a bug: some
    echo           early-return path is skipping the report.
    if exist "%HOUT%" del "%HOUT%"
    exit /b 1
)

echo UIGOLDEN: PASS - %PASSED% harness checks, 0 failed.
if exist "%HOUT%" del "%HOUT%"
exit /b 0

REM ===========================================================================
REM MANUAL CHECKLIST - not automated, and not automatable from here.
REM
REM Everything below needs a human looking at a real window. It is written out
REM rather than left in someone's head so it cannot quietly rot, and so a
REM future pass knows these were considered and not merely forgotten.
REM
REM   1. Bare launch (double-click, or run with no arguments): the window
REM      appears centred, with the list populated within one refresh.
REM   2. Theme: switch Windows between light and dark while the app is open.
REM      The list, status bar and filter box follow without a restart
REM      (WM_SYSCOLORCHANGE / WM_THEMECHANGED).
REM   3. High contrast: enable a high-contrast scheme. Every highlight colour
REM      must come from COLOR_HIGHLIGHT / COLOR_WINDOW, not a hard-coded one.
REM   4. DPI: drag the window between a 100% and a 150% monitor. Widths and
REM      font re-derive on WM_DPICHANGED; nothing truncates or overlaps.
REM   5. Tray: minimise to tray, then double-click the icon to restore. The
REM      tray menu's always-on-top toggle works and persists.
REM   6. Traffic columns: enable them, generate known traffic, and confirm the
REM      numbers MOVE. This is the one manual check that overlaps D2 - if the
REM      status bar reports timed-out scans, the columns are partial by design
REM      and the values are honest about it, but a human should confirm the
REM      column does not silently read a stale total as a current one.
REM   7. Details window: double-click a row. It opens centred over the main
REM      window, scales with DPI, and refreshes while a watch is running.
REM   8. Performance graphs: open them, let them run a minute, switch tabs.
REM      The four panels keep drawing and do not leak GDI handles (check with
REM      the task manager's GDI column over a long run).
REM   9. Change log: enable it, cause some churn, confirm APPEAR / DISAPPEAR /
REM      STATE rows appear with a timestamp and are append-only across restarts.
REM  10. Elevation relaunch: from an unelevated shell, trigger a feature that
REM      needs admin and decline the UAC prompt. The app must say the
REM      permission was declined, not fail silently.
REM  11. PARITY - both former CLI-only capabilities HAVE a GUI surface since
REM      2026-09-30 / 2026-10-01, so verify the surfaces rather than the gap:
REM        - bookmark notes: View > Columns > Note (hidden by default); a note
REM          is also on the Bookmarks column tooltip. Check both, and that the
REM          column sorts annotated rows first;
REM        - `--event appear,disappear,state`: the change-log window has three
REM          checkboxes (Appear / Disappear / State). Untick one and confirm new
REM          events of that kind stop arriving while the old ones stay - the
REM          mask is not retroactive by design, and the status line says so.
REM      The invariant that decided both: the GUI RENDERS, it never computes what a
REM      command computes - so a CLI-only feature is acceptable, a GUI
REM      reimplementation of it is not.
REM ===========================================================================
