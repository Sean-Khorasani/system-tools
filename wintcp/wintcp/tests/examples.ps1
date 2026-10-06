<#
.SYNOPSIS
    Executes every command listed in examples.txt against the real binary
    and reports any the product rejects.

.DESCRIPTION
    Invoked by examples.bat, which owns the reporting and the exit code.
    This is a separate file because a batch `for /f` loop cannot carry a
    nested double quote, and the documentation legitimately contains one:

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
$script:Expected = 0

function Invoke-Documented {
    param([string] $ArgsLine)

    # Skip blanks and comments so the command file can be annotated.
    $t = $ArgsLine.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { return }

    # `;expect=N` - an ANNOTATION, stripped before the binary ever sees the line.
    #
    # WHY IT EXISTS. This harness treats rc 2 as a failure, because for almost
    # every documented command an exit 2 means the documentation has drifted into
    # showing a command the product rejects. But some documented commands
    # CORRECTLY return 2 - `kill --pid 4` is documented as "always refused", and
    # `export --changes` is documented as printing the number 2. Those two were
    # left out of examples.txt entirely, because including them would fail the
    # gate for behaving exactly as documented. So no gate linked those specific
    # documented lines to anything, which is the gap this closes.
    #
    # Why a trailing annotation and not a second column or a prefix: `;` appears
    # in no documented command (checked, not assumed), so it cannot collide with
    # a command's own text, and a reader looking at the line sees the command
    # first and the annotation last.
    $expect = $null
    if ($t -match '^(.*?)\s*;\s*expect\s*=\s*(\d+)\s*$') {
        $t = $Matches[1].Trim()
        $expect = [int] $Matches[2]
    }

    $script:Runs++
    # Let the shell do the argument splitting, exactly as a reader's shell
    # would. `cmd /c` is used rather than a direct call so the binary sees the
    # same command line the documentation shows.
    $out = & cmd /c "`"$Bin`" $t 2>&1"
    $rc = $LASTEXITCODE

    if ($expect -ne $null) {
        $script:Expected++
        if ($rc -eq $expect) {
            Write-Host "ok  [$rc]  $t   (expected $expect)"
        } else {
            # Counted as a rejection EITHER way, because both directions are a
            # documentation defect: a command that stopped being refused is as
            # wrong as one that started being refused.
            Write-Host "REJECTED  $t   (expected $expect, got $rc)"
            foreach ($l in $out) { Write-Host "          $l" }
            $script:Rejections++
        }
        return
    }

    # rc 2 is the only failure this harness hunts: bad arguments. rc 1 means
    # "no live row matched", which is a correct answer to a filter question and
    # must not fail the gate - the studies are examples, not fixtures, and the
    # The documentation says so.
    if ($rc -eq 2) {
        Write-Host "REJECTED  $t"
        foreach ($l in $out) { Write-Host "          $l" }
        $script:Rejections++
    } else {
        Write-Host "ok  [$rc]  $t"
    }
}

# ---- self-test: prove this harness can actually FAIL --------------------------
Write-Host "EXAMPLES: self-test - proving the harness detects a rejection"
# BOTH commands run before either verdict is read. The first attempt checked
# the rejection count between the two calls, so the second never ran and the
# "ran 1 command, expected 2" check below fired on its own harness. A
# self-test that cannot reach its own second assertion is not a self-test.
$script:Rejections = 0
$script:Runs = 0
$script:Expected = 0
Invoke-Documented 'list --pid 1'              # must be REJECTED (rc 2)
Invoke-Documented 'list --filter "tcp:" --quiet'  # must be accepted (rc 0)
# Two more, for the `;expect=N` annotation. Both directions matter, and the
# second is the one that would otherwise be untested:
#   A - the rc-2 command WITH `;expect=2` must PASS. That is the whole feature:
#        without it, a documented command which correctly refuses cannot be
#        gated at all.
#   B - the rc-0 command with `;expect=2` must FAIL. Without this, an
#        annotation that simply ignored the expected value would pass case A and
#        look correct while checking nothing.
Invoke-Documented 'list --pid 1 ;expect=2'
Invoke-Documented 'list --filter "tcp:" --quiet ;expect=2'
if ($script:Runs -ne 4) {
    Write-Host ""
    Write-Host "EXAMPLES: FAIL - the self-test ran $($script:Runs) command(s), expected 4."
    Write-Host "COUNTS: 0 1"
    exit 1
}
if ($script:Expected -ne 2) {
    Write-Host ""
    Write-Host "EXAMPLES: FAIL - the self-test's two annotated commands registered"
    Write-Host "          $($script:Expected) expectation(s), expected 2. The"
    Write-Host "          'expect=N' annotation is not being parsed, so every"
    Write-Host "          annotated command in the real run below is running"
    Write-Host "          UNANNOTATED."
    Write-Host "COUNTS: 0 1"
    exit 1
}
if ($script:Rejections -ne 2) {
    Write-Host ""
    Write-Host "EXAMPLES: FAIL - the self-test's known-bad command (`list --pid 1`)"
    Write-Host "          was NOT detected as a rejection ($($script:Rejections) rejection(s)"
    Write-Host "          from 4 commands, expected 2: the unannotated one, plus the"
    Write-Host "          annotated rc-0 command that was told to expect 2 and did not"
    Write-Host "          get it. This harness is not measuring what it claims to, so"
    Write-Host "          every verdict below is meaningless. Aborting rather than"
    Write-Host "          reporting a number that would be false."
    Write-Host "COUNTS: 0 1"
    exit 1
}
Write-Host "EXAMPLES: self-test ok - a known rc-2 rejection is detected and a"
Write-Host "                 known rc-0 run is not. The verdict below is meaningful."
Write-Host ""

# ---- the real run -------------------------------------------------------------
$script:Rejections = 0
$script:Runs = 0
$script:Expected = 0
foreach ($line in [System.IO.File]::ReadAllLines($CmdFile)) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    Invoke-Documented $t
}
$n = $script:Runs
$bad = $script:Rejections
$exp = $script:Expected

# Machine-readable tail, parsed by the .bat wrapper. Kept last and on its own
# line so the wrapper never has to interpret a human sentence.
Write-Host "COUNTS: $n $bad"
# How many of the $n carried a `;expect=N` annotation. Printed so a reader can
# see that the annotated commands are actually being run rather than silently
# skipped by the annotation parser - which is the failure mode an annotation
# feature has by default: a typo in the annotation that never matches leaves
# the line running UNANNOTATED, and rc 2 then fails the gate for a command that
# is behaving correctly.
if ($exp -gt 0) { Write-Host "ANNOTATED: $exp of $n commands carried ;expect=N" }
# No ternary: this runs on Windows PowerShell 5.1, where `? :` is a parse
# error. The `exit` value is the wrapper's real signal; the COUNTS line is
# there so a human reading the log sees the numbers too.
if ($bad -eq 0) { exit 0 }
exit 1
