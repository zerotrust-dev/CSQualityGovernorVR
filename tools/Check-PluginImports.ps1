<#
.SYNOPSIS
Fails if the built SKSE plugin imports DLLs that will not be present at runtime.

.DESCRIPTION
An SKSE plugin that imports spdlog.dll or fmt.dll loads fine on the build
machine and fails in the game with:

    couldn't load plugin ... (Error 126)

Error 126 is ERROR_MOD_NOT_FOUND - the plugin itself was found, but one of its
dependencies was not. It cost a wasted game session on 2026-08-03, caused by
vcpkg defaulting to the dynamic x64-windows triplet instead of
x64-windows-static-md.

The symptom is invisible until the game rejects the plugin, so CI checks for it.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [string[]]$Forbidden = @('spdlog.dll', 'fmt.dll', 'zlib.dll', 'zlib1.dll')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Path)) { throw "not found: $Path" }

$bytes = [IO.File]::ReadAllBytes($Path)
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
$magic = [BitConverter]::ToUInt16($bytes, $peOff + 0x18)
$numSections = [BitConverter]::ToUInt16($bytes, $peOff + 6)
$optSize = [BitConverter]::ToUInt16($bytes, $peOff + 0x14)
$dataDir = $peOff + 0x18 + $(if ($magic -eq 0x20B) { 112 } else { 96 })
$importRva = [BitConverter]::ToUInt32($bytes, $dataDir + 8)
$sectionStart = $peOff + 0x18 + $optSize

function Convert-RvaToOffset {
    param([uint32]$Rva)
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $sectionStart + $i * 40
        $va = [BitConverter]::ToUInt32($bytes, $s + 12)
        $vsize = [BitConverter]::ToUInt32($bytes, $s + 8)
        $raw = [BitConverter]::ToUInt32($bytes, $s + 20)
        if ($Rva -ge $va -and $Rva -lt ($va + [Math]::Max($vsize, 1))) {
            return $raw + ($Rva - $va)
        }
    }
    return 0
}

$imports = @()
if ($importRva -ne 0) {
    $off = Convert-RvaToOffset $importRva
    while ($true) {
        $nameRva = [BitConverter]::ToUInt32($bytes, $off + 12)
        if ($nameRva -eq 0) { break }
        $nameOff = Convert-RvaToOffset $nameRva
        $end = $nameOff
        while ($bytes[$end] -ne 0) { $end++ }
        $imports += [Text.Encoding]::ASCII.GetString($bytes, $nameOff, $end - $nameOff)
        $off += 20
    }
}

Write-Host "Imports of $(Split-Path $Path -Leaf):" -ForegroundColor Cyan
$imports | ForEach-Object { Write-Host "  $_" }

$bad = $imports | Where-Object { $Forbidden -contains $_.ToLower() }
if ($bad) {
    Write-Host ""
    Write-Host "FAIL: the plugin imports DLLs that are not shipped and will not be" -ForegroundColor Red
    Write-Host "      present at runtime. The game will reject it with Error 126." -ForegroundColor Red
    $bad | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "      Almost certainly a vcpkg triplet problem. It must be" -ForegroundColor Yellow
    Write-Host "      x64-windows-static-md, not the default x64-windows." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "OK: no dynamically linked third-party runtime dependencies." -ForegroundColor Green
exit 0
