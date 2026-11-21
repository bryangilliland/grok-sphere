@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo Run setup_windows.bat first.
  pause
  exit /b 1
)

".venv\Scripts\python.exe" -m pip install pyinstaller
".venv\Scripts\python.exe" -m PyInstaller --noconfirm --onedir --name ThadSphereCompanion ^
  --collect-all faster_whisper --collect-all ctranslate2 --collect-all edge_tts ^
  --add-data "config.yaml;." run_companion.py

echo.
echo Built dist\ThadSphereCompanion\ThadSphereCompanion.exe
echo Copy your .env file next to ThadSphereCompanion.exe before running it.
pause
