@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\rollback.ps1" %*
pause
