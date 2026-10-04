<#
.SYNOPSIS
    Executes every command listed in readme_cmds.txt against the real binary
    and reports any the product rejects.

.DESCRIPTION
    Invoked by readmegolden.bat, which owns the reporting and the exit code.
    This is a separate file because a batch `for /f` loop cannot carry a
    nested double quote, and the README legitimately contains one:

        --filter "note:""corporate dns"""

    which the batch loop truncated to `--filter "note:""corporate` - silently
    turning a valid documented command into a rejected one. A false FAILURE is
    tolerable; a harness that has to be special-cased for its own input file is
    not. PowerShell's argument parser resolves the quoting the way a reader's
    shell does, which is exactly the property under test.

.NOTES
    The self-test at the top is the important part. The first version of this
    harness (as a batch file) reported success for every input including
    commands the binary rejects, because it compared an unsubstituted !RC!
    against "2". A checker that has never been seen to fail is not a checker,
    so before reporting anything this script proves it can detect a known
    rejection - and aborts, loudly, if it cannot.
#>
param(
    [Parameter(Mandatory = $true)][string] $Bin,
    [Parameter(Mandatory = $true)][string] $CmdFile
)

$ErrorActionPreference = 'Continue'

# Verdicts travel in a script-scoped variable, NOT as a function return value.
#
# This is the second trap in this file, and it is the same shape as the first:
# in PowerShell a function's `Write-Output` lines and its `return` value share
# one pipeline, so `$v = Invoke-X` captures the human text AND the verdict as
# an array - and `if ($v -ne 1)` then tests an array, which is truthy for any
# element that is not 1, i.e. almost always. The first attempt at this file
# failed its own self-test for exactly that reason: it correctly printed
# "REJECTED" and then reported that the rejection had NOT been detected.
# Write-Host goes to the console and leaves the pipeline alone; a script-scoped
# variable carries the verdict. Both are unambiguous.
$script:Rejections = 0
$script:Runs = 0

function Invoke-Documented {
    param([string] $ArgsLine)

    # Skip blanks and comments so the command file can be annotated.
    $t = $ArgsLine.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { return }

    $script:Runs++
    # Let the shell do the argument splitting, exactly as a reader's shell
    # would. `cmd /c` is used rather than a direct call so the binary sees the
    # same command line the documentation shows.
    $out = & cmd /c "`"$Bin`" $t 2>&1"
    $rc = $LASTEXITCODE

    # rc 2 is the only failure this harness hunts: bad arguments. rc 1 means
    # "no live row matched", which is a correct answer to a filter question and
    # must not fail the gate - the studies are examples, not fixtures, and the
    # README says so.
    if ($rc -eq 2) {
        Write-Host "REJECTED  $t"
        foreach ($l in $out) { Write-Host "          $l" }
        $script:Rejections++
    } else {
        Write-Host "ok  [$rc]  $t"
    }
}

# ---- self-test: prove this harness can actually FAIL --------------------------
Write-Host "READMEGOLDEN: self-test - proving the harness detects a rejection"
# BOTH commands run before either verdict is read. The first attempt checked
# the rejection count between the two calls, so the second never ran and the
# "ran 1 command, expected 2" check below fired on its own harness. A
# self-test that cannot reach its own second assertion is not a self-test.
$script:Rejections = 0
$script:Runs = 0
Invoke-Documented 'list --pid 1'              # must be REJECTED (rc 2)
Invoke-Documented 'list --filter "tcp:" --quiet'  # must be accepted (rc 0)
if ($script:Runs -ne 2) {
    Write-Host ""
    Write-Host "READMEGOLDEN: FAIL - the self-test ran $($script:Runs) command(s), expected 2."
    Write-Host "COUNTS: 0 1"
    exit 1
}
if ($script:Rejections -ne 1) {
    Write-Host ""
    Write-Host "READMEGOLDEN: FAIL - the self-test's known-bad command (`list --pid 1`)"
    Write-Host "          was NOT detected as a rejection ($($script:Rejections) rejection(s)"
    Write-Host "          from 2 commands, expected 1). This harness is not measuring"
    Write-Host "          what it claims to, so every verdict below is meaningless."
    Write-Host "          Aborting rather than reporting a number that would be false."
    Write-Host "COUNTS: 0 1"
    exit 1
}
Write-Host "READMEGOLDEN: self-test ok - a known rc-2 rejection is detected and a"
Write-Host "                 known rc-0 run is not. The verdict below is meaningful."
Write-Host ""

# ---- the real run -------------------------------------------------------------
$script:Rejections = 0
$script:Runs = 0
foreach ($line in [System.IO.File]::ReadAllLines($CmdFile)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    Invoke-Documented $t
}
$n = $script:Runs
$bad = $script:Rejections

# Machine-readable tail, parsed by the .bat wrapper. Kept last and on its own
# line so the wrapper never has to interpret a human sentence.
Write-Host "COUNTS: $n $bad"
# No ternary: this runs on Windows PowerShell 5.1, where `? :` is a parse
# error. The `exit` value is the wrapper's real signal; the COUNTS line is
# there so a human reading the log sees the numbers too.
if ($bad -eq 0) { exit 0 }
exit 1
