@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_configure_build.ps1" %*
