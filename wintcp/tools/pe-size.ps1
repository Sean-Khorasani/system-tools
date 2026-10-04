# tools/pe-size.ps1 - PE section + import measurement for wintcp.exe
#
# WHY THIS EXISTS. dumpbin works, but its output is awkward to parse reliably
# (the section table interleaves name/virtual-size/virtual-address rows, and a
# naive "find the line with a dot" grep misses sections whose name row wraps).
# This reads the PE headers directly so the numbers can be diffed mechanically
# between builds, which is the whole point of a size audit.
#
# Usage:
#   powershell -NoProfile -File tools\pe-size.ps1 build\wintcp.exe [more.exe ...]
#   powershell -NoProfile -File tools\pe-size.ps1 -Json build\wintcp.exe
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Path,
      [switch]$Json)

$ErrorActionPreference = 'Stop'

function Read-Pe([string]$file) {
    $b = [System.IO.File]::ReadAllBytes($file)
    $lfanew = [BitConverter]::ToInt32($b, 0x3C)
    $nSections = [BitConverter]::ToUInt16($b, $lfanew + 6)
    $optSize   = [BitConverter]::ToUInt16($b, $lfanew + 20)
    $opt       = $lfanew + 24
    $magic     = [BitConverter]::ToUInt16($b, $opt)
    $is64      = $magic -eq 0x20B

    # Data directories: index 1 = imports, 5 = reloc, 6 = debug.
    $dirBase = if ($is64) { $opt + 112 } else { $opt + 96 }
    $numDirs = [BitConverter]::ToUInt32($b, $opt + $(if ($is64) { 108 } else { 92 }))
    $dirs = @()
    for ($i = 0; $i -lt [Math]::Min($numDirs, 16); $i++) {
        $dirs += [pscustomobject]@{
            RVA  = [BitConverter]::ToUInt32($b, $dirBase + $i * 8)
            Size = [BitConverter]::ToUInt32($b, $dirBase + $i * 8 + 4)
        }
    }

    $secBase = $opt + $optSize
    $secs = @()
    for ($i = 0; $i -lt $nSections; $i++) {
        $o = $secBase + $i * 40
        $name = ([System.Text.Encoding]::ASCII.GetString($b, $o, 8)).Trim([char]0)
        $secs += [pscustomobject]@{
            Name     = $name
            VirtSize = [BitConverter]::ToUInt32($b, $o + 8)
            VirtAddr = [BitConverter]::ToUInt32($b, $o + 12)
            RawSize  = [BitConverter]::ToUInt32($b, $o + 16)
            RawPtr   = [BitConverter]::ToUInt32($b, $o + 20)
            Chars    = [BitConverter]::ToUInt32($b, $o + 36)
        }
    }

    # Import DLL names, read straight out of the import table.
    #
    # RVA -> file offset needs the section that CONTAINS the RVA. The first
    # version of this walked `while ($s.VirtAddr -le $rva -and
    # ($s.VirtAddr + $s.VirtSize) -gt $rva) { break }`, which loops forever
    # because a `while` with a constant-true body never exits - it then read
    # thunk pointers as if they were DLL names, which is where the garbage
    # "imports" list came from. This version resolves the section first and
    # validates that the name RVA lands inside the image at all, so a malformed
    # table yields an empty list rather than nonsense.
    $imports = @()
    if ($dirs.Count -gt 1 -and $dirs[1].RVA -ne 0) {
        $rva = $dirs[1].RVA
        $sec = $secs | Where-Object {
            $_.VirtAddr -le $rva -and ($_.VirtAddr + [Math]::Max($_.VirtSize, $_.RawSize)) -gt $rva
        } | Select-Object -First 1
        if ($sec) {
            $base = $sec.RawPtr - $sec.VirtAddr
            $p = $rva + $base
            $guard = 0
            while ($p -gt 0 -and $p -lt ($b.Length - 24) -and $guard -lt 128) {
                $guard++
                # A null descriptor terminates the list.
                if ([BitConverter]::ToInt32($b, $p) -eq 0) { break }
                $nameRva = [BitConverter]::ToUInt32($b, $p + 12)
                if ($nameRva -eq 0) { $p += 20; continue }
                $nameOff = $nameRva + $base
                if ($nameOff -lt 0 -or $nameOff -ge $b.Length) { $p += 20; continue }
                $end = $nameOff
                while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
                if ($end -le $nameOff) { $p += 20; continue }
                $name = [System.Text.Encoding]::ASCII.GetString($b, $nameOff, $end - $nameOff)
                # A DLL name is printable ASCII ending in .dll; anything else
                # means we walked off the table and should not report it.
                if ($name -match '^[A-Za-z0-9._\-]+\.dll$') { $imports += $name }
                $p += 20
            }
        }
    }

    return [pscustomobject]@{
        File     = $file
        Length   = $b.Length
        Sections = $secs
        Imports  = $imports
        HasReloc = ($dirs.Count -gt 5 -and $dirs[5].RVA -ne 0)
        DebugSize = if ($dirs.Count -gt 6) { $dirs[6].Size } else { 0 }
    }
}

$peFiles = @()
foreach ($p in $Path) { $peFiles += Read-Pe $p }

if ($Json) {
    $peFiles | ForEach-Object {
        [pscustomobject]@{
            file     = $_.File
            bytes    = $_.Length
            sections = $_.Sections
            imports  = $_.Imports
            hasReloc = $_.HasReloc
        }
    } | ConvertTo-Json -Depth 5
    exit 0
}

foreach ($pe in $peFiles) {
    Write-Output "=== $($pe.File) : $([math]::Round($pe.Length/1KB,1)) KB ==="
    Write-Output ("  {0,-10} {1,12} {2,12}" -f 'section', 'vsize', 'rawsize')
    foreach ($s in $pe.Sections) {
        Write-Output ("  {0,-10} {1,12} {2,12}" -f $s.Name, $s.VirtSize, $s.RawSize)
    }
    $code = ($pe.Sections | Where-Object { $_.Name -eq '.text' }).RawSize
    $rdata = ($pe.Sections | Where-Object { $_.Name -eq '.rdata' }).RawSize
    Write-Output ("  .text  = {0} KB   .rdata = {1} KB" -f [math]::Round($code/1KB,1), [math]::Round($rdata/1KB,1))
    Write-Output ("  imports ({0}): {1}" -f $pe.Imports.Count, ($pe.Imports -join ' '))
    Write-Output ("  reloc present: {0}    debug dir bytes: {1}" -f $pe.HasReloc, $pe.DebugSize)
    Write-Output ""
}