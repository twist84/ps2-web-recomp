@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_regenerate_units.ps1" %*
