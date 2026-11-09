@echo off
rem Removes Kyubu from a Cube World folder. Usage: uninstall.cmd ["C:\path\to\Cube World"] [-Purge]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kyubu_install.ps1" -Action uninstall %*
pause
