@echo off
cd /d "%~dp0"
python restore_binaries.py
if errorlevel 1 python3 restore_binaries.py
pause
