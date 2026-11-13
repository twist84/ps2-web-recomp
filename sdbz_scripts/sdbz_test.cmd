@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdbz_play.ps1" %*
