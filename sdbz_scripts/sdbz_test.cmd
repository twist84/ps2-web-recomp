@echo off
setlocal
set SDBZ_ARKD_NATIVE=1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_play.ps1" %*
