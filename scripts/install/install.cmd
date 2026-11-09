@echo off
rem Installs Kyubu into a Cube World folder. Usage: install.cmd ["C:\path\to\Cube World"]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kyubu_install.ps1" -Action install %*
pause
