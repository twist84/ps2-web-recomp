$ErrorActionPreference = 'Stop'

$Root = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..\sdbz")
$ProjectRoot = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..")
$BuildDir = "$Root\build-clang"
$Dest = "$Root\games\SuperDragonBallZ\disc"

Set-Location -LiteralPath $ProjectRoot

# Ensure build-clang is configured (tools built, units generated, PS2_GENERATED set).
$cache = Join-Path -Path $BuildDir -ChildPath "CMakeCache.txt"
$Configured = if (Test-Path -LiteralPath $cache) {
    $null -ne (Get-Content -LiteralPath $cache | Select-String "PS2_GENERATED:")
} else { $false }

if (-not $Configured) {
    Write-Host "build-clang not configured; running sdbz_configure_build..."
    & $PSScriptRoot\sdbz_configure_build.ps1
    if ($LASTEXITCODE -ne 0) { throw "sdbz_configure_build_clang failed" }
}

Set-Location -LiteralPath $BuildDir

Write-Host "Building SDBZ with ninja..."
ninja

Write-Host "Copying artifacts to $Dest..."
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

Copy-Item -Path "$BuildDir\SDBZ.exe" -Destination $Dest -Force
Copy-Item -Path "$BuildDir\SDL2.dll" -Destination $Dest -Force
Copy-Item -Path "$BuildDir\wgpu_native.dll" -Destination $Dest -Force

Write-Host "Done."
