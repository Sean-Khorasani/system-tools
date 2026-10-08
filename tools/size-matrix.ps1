param([string]$OutFile = "temp\size-matrix.txt")
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$root = (Get-Location).Path

# THE WHOLE COMMAND IS PARSED FROM build.bat, NOT COPIED FROM IT.
#
# Both halves used to be hand-transcribed here, and the transcription had
# drifted: this file's baseline carried `/O2` and nothing else, while
# build.bat's product line also has `/MT /Gy /GL /GF /Os`, `delayimp.lib`,
# six `/DELAYLOAD:` entries and `/LTCG /OPT:REF /OPT:ICF /DEBUG:NONE`. So
# `01-base-as-is` measured a 1189 KB program against a shipped 990 KB one and
# called the difference a baseline - 200 KB of unlabelled flag, on a chart
# whose every row is a delta. The source list had already caused this exact
# failure once (nine identical LNK1121/2001 failures that read as a size
# problem and were a stale-list problem), and a matrix that measures a
# different program than the one that ships is worse than no matrix.
$b = Get-Content (Join-Path $root 'build.bat')

function Find-Line([string]$pattern) {
    for ($i = 0; $i -lt $b.Count; $i++) { if ($b[$i] -match $pattern) { return $i } }
    return -1
}

$clIdx   = Find-Line '^cl /nologo'
$srcIdx  = Find-Line 'src\\main\.cpp'
# NOT simply 'wintcp.res': build.bat's rc line is `rc /nologo /fo
# "build\wintcp.res" "wintcp\src\wintcp.rc"` and sits at idx 55, BEFORE the cl
# block, so the naive pattern matches it and inverts the order check below.
# This matches only a bare quoted .res argument on its own continuation line
# - the "build\wintcp.res" that cl actually feeds to the linker.
$resIdx  = Find-Line '^\s*"[^"]*\.res"\s*\^?\s*$'
$linkIdx = Find-Line '/link /SUBSYSTEM'
if ($clIdx -lt 0 -or $srcIdx -lt 0 -or $resIdx -lt 0 -or $linkIdx -lt 0) {
    throw "could not find the cl / source / resource / link lines in build.bat"
}
# ORDER IS CHECKED, not assumed: build.bat has a second cl line for the test
# driver, so taking the first match of every field is only correct while the
# product block comes first in all four. If someone reorders it, this throws
# instead of silently mixing the product's flags with the test driver's.
if (-not ($clIdx -lt $srcIdx -and $srcIdx -lt $resIdx -and $resIdx -lt $linkIdx)) {
    throw "build.bat product cl block is no longer ordered cl < sources < res < link"
}

$clFlags   = $b[$clIdx].Trim()   -replace '^cl\s+', '' -replace '\s*\^\s*$', ''
$resFile   = $b[$resIdx].Trim()  -replace '\s*\^\s*$', ''
$linkFlags = $b[$linkIdx].Trim() -replace '\s*\^\s*$', ''
$srcLine = $b[$srcIdx]
if (-not $srcLine) { throw "could not find the source list in build.bat" }
$srcs = ([regex]::Matches($srcLine, '"([^"]+\.cpp)"') |
         ForEach-Object { '"' + $_.Groups[1].Value + '"' }) -join ' '
Write-Host ("size-matrix: " + ($srcs.Split(' ').Count) + " sources, flags and " +
            "link line all parsed from build.bat")

# THE OPTIMISATION LEVEL USED TO BE A HAND-WRITTEN INVARIANT, AND ITS ABSENCE
# INVALIDATED A WHOLE SWEEP: the first version of this file omitted /O2, so
# every variant compiled at the default /Od, came out LARGER than the shipped
# binary, and reported "01-base-as-is 1884 KB" against a real 1473 KB - which
# made /O1 look like a 446 KB win when it was only beating an unoptimised
# build. Now the flags come from build.bat, so this is no longer a comment
# asking a future reader to keep two copies in sync: it is an assertion that
# fails the run if parsing ever stops finding them.
if ($clFlags -notmatch '/O2' -or $clFlags -notmatch '/MT' -or
    $linkFlags -notmatch '/link ') {
    throw "parsed flags from build.bat are missing /O2, /MT or /link - refusing to measure"
}
# build.bat links this too, and a variant missing it would fail LNK1120/2001
# in a way that looks like a size result rather than a broken harness.
if ($resFile -notmatch '\.res"?$') {
    throw "parsed resource line from build.bat does not name a .res file"
}

$variants = @(
  @{ n='01-base-as-is';        f='' },
  @{ n='02-md';                f='/MD' },
  @{ n='03-md-gy';             f='/MD /Gy' },
  @{ n='04-md-gy-ltcg';        f='/MD /Gy /GL /LTCG' },
  @{ n='05-md-gy-ltcg-gf-gr';  f='/MD /Gy /GL /LTCG /GF /GR-' },
  @{ n='06-plus-os';           f='/MD /Gy /GL /LTCG /GF /GR- /Os' },
  @{ n='07-os-jmc-nocf';       f='/MD /Gy /GL /LTCG /GF /GR- /Os /JMC- /guard:cf-' },
  @{ n='08-mt-gy-ltcg-os';     f='/MT /Gy /GL /LTCG /GF /GR- /Os /JMC-' },
  @{ n='09-o1';                f='/MD /Gy /GL /LTCG /GF /GR- /O1' },
  @{ n='10-os-merged';         f='/MD /Gy /GL /LTCG /GF /GR- /Os /JMC- /MERGE:.rdata=.text' },
  @{ n='11-md-nos-cf';         f='/MD /Gy /GL /LTCG /GF /GR- /Os /JMC- /guard:cf-' }
)

# THE DELTA REFERENCE IS THE SHIPPED BINARY, NOT A CONSTANT. It used to be
# 1508352 (1473 KB) - the size of some past build, typed in and never updated,
# so after any real change every "delta" on the chart was an offset from a
# number that no longer described anything. Measuring build\wintcp.exe (what
# build.bat actually just produced) makes the delta mean "vs. what ships",
# which is the only comparison any of these rows is useful for. If the binary
# is missing the run still proceeds, but says so instead of silently
# reintroducing a hardcoded figure.
$refBytes = $null
$shipExe = Join-Path $root 'build\wintcp.exe'
if (Test-Path $shipExe) {
    $refBytes = (Get-Item $shipExe).Length
    Write-Host ("size-matrix: delta reference = build\wintcp.exe = {0} bytes" -f $refBytes)
} else {
    Write-Warning "size-matrix: build\wintcp.exe not found - deltas will be measured against the baseline variant instead"
}

$rows = @()
foreach ($v in $variants) {
  # The suffix keeps EVERY alphanumeric char, not just digits. With digits
  # only, '09-o1' collapsed to '091' (and '10-os-merged' to '10'), so variant
  # names collided into the same directory whenever their digits matched -
  # each one then deleted the other's output before rebuilding. Sanitizing the
  # whole name makes the directory one-to-one with the variant.
  $dir = "build\M" + ($v.n -replace '[^0-9A-Za-z]', '')
  if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  # /Fo takes the DIRECTORY, /Fe takes the full exe path.
  #
  # WHY A GENERATED .bat INSTEAD OF A DIRECT cl INVOCATION. Passing this line
  # through Start-Process cmd.exe /c lost the embedded quotes: the shell saw
  # /Fobuild\M01\ (quotes already gone) and cl rejected it with D8036 "not
  # allowed with multiple source files" - nine identical, instant failures
  # that looked like a flag problem and were purely a quoting one. Writing the
  # line into a real .bat and running THAT keeps the quotes intact, because
  # batch is the thing that understands them.
  $bat = "temp\M_" + ($v.n -replace '[^A-Za-z0-9]','_') + ".bat"
  # /Fo takes a DIRECTORY and cl only accepts one in the exact form build.bat
  # uses: /Fo"dir/" with a FORWARD slash inside the quotes.
  #
  # Two syntax traps, both cost a full sweep of nine identical instant
  # failures, and both produce error text that points at the wrong thing:
  #   * /Fo"dir"  (no slash)  -> D8036 "'/Fo<dir>' not allowed with multiple
  #     source files", which reads like a flag mistake rather than syntax.
  #   * /Fo"dir\" (backslash) -> cmd treats the backslash as escaping the
  #     closing quote, so everything after it is swallowed as ONE argument:
  #     D8038 "invalid argument 'dir" /Febuild/M01/wintcp.exe wintcp\src\...'".
  $foDir = ($dir -replace '\\', '/')
  # $clFlags already starts with /nologo, so it is not repeated here. The
  # variant's own flags come after build.bat's on purpose: cl reports the
  # resulting override as command-line warning D9025 and, because D9025 is not
  # a compile diagnostic, /W4 /WX does NOT escalate it - so /MT -> /MD and
  # /O2 -> /O1 land silently with the last flag winning, which is what a
  # variant overlay needs. (Verified: exit 0 with and without /WX.)
  $line = "@echo off`r`ncl $clFlags $($v.f) /Fo`"$foDir/`" /Fe`"$dir\wintcp.exe`" " +
          $srcs + ' ' + $resFile + ' ' + $linkFlags + "`r`n"
  [System.IO.File]::WriteAllText((Join-Path $root $bat), $line)
  $log = "temp\M_" + ($v.n -replace '[^A-Za-z0-9]','_') + ".log"
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $bat `
        -WorkingDirectory $root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
  $p.WaitForExit()
  $sw.Stop()
  $exe = Join-Path $dir 'wintcp.exe'
  if (Test-Path $exe) {
    $sz = (Get-Item $exe).Length
    # First successful build becomes the reference when build\wintcp.exe was
    # absent, so the chart is still self-consistent rather than absolute-zero.
    if ($null -eq $refBytes) { $refBytes = $sz }
    $rows += [pscustomobject]@{
      Variant = $v.n; Bytes = $sz; KB = [math]::Round($sz/1KB,1)
      DeltaKB = [math]::Round(($sz - $refBytes)/1KB,1); Sec = [math]::Round($sw.Elapsed.TotalSeconds,0)
    }
  } else {
    $err = (Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 2) -join ' '
    $rows += [pscustomobject]@{
      Variant = $v.n; Bytes = 'BUILD FAILED'; KB = ''; DeltaKB = ''
      Sec = [math]::Round($sw.Elapsed.TotalSeconds,0)
    }
    Add-Content $OutFile "  !! $($v.n): $err"
  }
  Add-Content $OutFile ("{0,-24} {1,12} {2,8} KB  {3,9} KB  {4,5}s" -f $v.n, $rows[-1].Bytes, $rows[-1].KB, $rows[-1].DeltaKB, $rows[-1].Sec)
  # Objects are large; drop them so 11 variants do not fill the disk.
  Get-ChildItem $dir -Filter *.obj -ErrorAction SilentlyContinue | Remove-Item -Force
}

$rows | Format-Table -AutoSize | Out-String