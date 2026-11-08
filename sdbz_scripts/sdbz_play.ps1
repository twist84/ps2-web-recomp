$ErrorActionPreference = 'Stop'

$Root = Resolve-Path -LiteralPath (Join-Path -Path $PSScriptRoot -ChildPath "..\sdbz")
$Disc = "$Root\games\SuperDragonBallZ\disc"
$SdbzExe = "$Disc\SDBZ.exe"

if (!(Test-Path -LiteralPath $SdbzExe)) { throw "SDBZ.exe not found: $SdbzExe" }

Set-Location -LiteralPath $Disc

Write-Host "Playing SDBZ..."
& $SdbzExe games/SuperDragonBallZ/disc/SLES_541.61 --window $args
exit $LASTEXITCODE
