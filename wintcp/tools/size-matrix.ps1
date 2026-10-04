param([string]$OutFile = "temp\size-matrix.txt")
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$root = (Get-Location).Path

# The compile line, copied from build.bat with ONLY the /Fo /Fe parts changed.
# The source list is PARSED OUT OF build.bat, not duplicated here. The first
# version of this file listed the translation units itself, which worked until
# WinCaps.cpp was added to build.bat and not here: every variant then failed
# LNK1121/2001 on WinCapabilities() - nine identical failures that read as a
# size problem and were a stale-list problem. A build matrix that measures a
# different program than the one that ships is worse than no matrix.
$b = Get-Content (Join-Path $root 'build.bat')
$srcLine = ($b | Where-Object { $_ -match 'src\\main\.cpp' } | Select-Object -First 1)
if (-not $srcLine) { throw "could not find the source list in build.bat" }
$srcs = ([regex]::Matches($srcLine, '"([^"]+\.cpp)"') |
         ForEach-Object { '"' + $_.Groups[1].Value + '"' }) -join ' '
Write-Host ("size-matrix: " + ($srcs.Split(' ').Count) + " sources parsed from build.bat")

# /O2 IS HERE, and its absence invalidated a whole sweep before it was found.
# The first run of this script omitted it, so every variant compiled at the
# default /Od and came out LARGER than the /O2 binary build.bat produces. That
# produced the nonsense row "01-base-as-is 1884 KB" when the real baseline is
# 1473 KB, and made /O1 look like a 446 KB WIN when it was really only beating
# an unoptimised build. The optimisation level belongs to the baseline because
# every one of these questions is "what does flag X cost ON TOP OF /O2".
$common = '/std:c++17 /EHsc /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0601 /DWIN32_LEAN_AND_MEAN /O2'
$libs = '/link /SUBSYSTEM:CONSOLE iphlpapi.lib ws2_32.lib comctl32.lib psapi.lib user32.lib gdi32.lib comdlg32.lib shell32.lib advapi32.lib crypt32.lib pdh.lib ole32.lib oleaut32.lib'

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

$rows = @()
foreach ($v in $variants) {
  $dir = "build\M" + ($v.n -replace '[^0-9]','')
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
  $line = "@echo off`r`ncl /nologo $common $($v.f) /Fo`"$foDir/`" /Fe`"$dir\wintcp.exe`" " +
          ($srcs -join ' ') + ' "build\wintcp.res" ' + $libs + "`r`n"
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
    $rows += [pscustomobject]@{
      Variant = $v.n; Bytes = $sz; KB = [math]::Round($sz/1KB,1)
      DeltaKB = [math]::Round(($sz - 1508352)/1KB,1); Sec = [math]::Round($sw.Elapsed.TotalSeconds,0)
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