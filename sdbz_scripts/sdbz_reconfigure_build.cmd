@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_reconfigure_build.ps1" %*
