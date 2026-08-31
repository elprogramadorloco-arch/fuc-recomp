@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File ".\tools\run_tui.ps1" %*
exit /b %errorlevel%
