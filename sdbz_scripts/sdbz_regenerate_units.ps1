$ErrorActionPreference = 'Stop'

$Root = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..\sdbz")
$ProjectRoot = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..")
$BuildDir = "$Root\build-clang"
$GenDir = "$Root\generated\SuperDragonBallZ"
$Elf = "$Root\games\SuperDragonBallZ\disc\SLES_541.61"
$Entries = "$Root\games\SuperDragonBallZ\entries.txt"
$Vu1Dir = "$Root\games\SuperDragonBallZ\vu1"

if (!(Test-Path -LiteralPath $Elf)) { throw "ELF not found: $Elf" }

Set-Location -LiteralPath $ProjectRoot

# Ensure build-clang is configured so eerecomp/vurecomp exist.
$cache = Join-Path -Path $BuildDir -ChildPath "CMakeCache.txt"
$Configured = if (Test-Path -LiteralPath $cache) {
    $null -ne (Get-Content -LiteralPath $cache | Select-String "PS2_GENERATED:")
} else { $false }

if (-not $Configured) {
    Write-Host "build-clang not configured; running sdbz_configure_build..."
    & $PSScriptRoot\sdbz_configure_build.ps1
    if ($LASTEXITCODE -ne 0) { throw "sdbz_configure_build_clang failed" }
}

$Eerecomp = "$BuildDir\eerecomp.exe"
$Vurecomp = "$BuildDir\vurecomp.exe"
if (!(Test-Path -LiteralPath $Eerecomp)) { throw "eerecomp.exe missing: $Eerecomp" }
if (!(Test-Path -LiteralPath $Vurecomp)) { throw "vurecomp.exe missing: $Vurecomp" }

Write-Host "Removing existing generated units..."
Remove-Item -LiteralPath $GenDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "Regenerating EE units..."
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
& $Eerecomp $Elf --entries $Entries --out $GenDir
if ($LASTEXITCODE -ne 0) { throw "eerecomp failed" }

Write-Host "Regenerating VU units..."
if (Test-Path -LiteralPath $Vu1Dir) {
    $bins = Get-ChildItem -LiteralPath $Vu1Dir -Filter *.bin -File
    if ($bins) {
        & $Vurecomp $Vu1Dir $GenDir
        if ($LASTEXITCODE -ne 0) { throw "vurecomp failed" }
    } else {
        Write-Host "No VU .bin programs recorded, skipping vurecomp."
    }
} else {
    Write-Host "No VU directory, skipping vurecomp."
}

Write-Host "Done."
