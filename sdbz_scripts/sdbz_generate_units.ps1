$ErrorActionPreference = 'Stop'

$Root = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..\sdbz")
$BuildDir = "$Root\build-clang"
$GenDir = "$Root\generated\SuperDragonBallZ"
$Elf = "$Root\games\SuperDragonBallZ\disc\SLES_541.61"
$Entries = "$Root\games\SuperDragonBallZ\entries.txt"
$Vu1Dir = "$Root\games\SuperDragonBallZ\vu1"

$Eerecomp = "$BuildDir\eerecomp.exe"
$Vurecomp = "$BuildDir\vurecomp.exe"

if (!(Test-Path -LiteralPath $Elf)) { throw "ELF not found: $Elf" }

Set-Location -LiteralPath $BuildDir

Write-Host "Ensuring eerecomp.exe and vurecomp.exe are built..."
if (!(Test-Path -LiteralPath $Eerecomp) -or !(Test-Path -LiteralPath $Vurecomp)) {
    ninja eerecomp vurecomp
    if ($LASTEXITCODE -ne 0) { throw "ninja build of eerecomp/vurecomp failed" }
}
if (!(Test-Path -LiteralPath $Eerecomp)) { throw "eerecomp.exe still missing after build: $Eerecomp" }
if (!(Test-Path -LiteralPath $Vurecomp)) { throw "vurecomp.exe still missing after build: $Vurecomp" }

Set-Location -LiteralPath $Root

Write-Host "Generating EE units..."
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
& $Eerecomp $Elf --entries $Entries --out $GenDir
if ($LASTEXITCODE -ne 0) { throw "eerecomp failed" }

Write-Host "Generating VU units..."
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
