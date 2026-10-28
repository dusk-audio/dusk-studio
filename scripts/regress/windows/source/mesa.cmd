@echo off
setlocal
call "%~dp0env.cmd" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify-mesa.ps1"
exit /b %ERRORLEVEL%
