@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_generate_units.ps1" %*
