@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File ".\tools\update_tui.ps1" %*
exit /b %errorlevel%
