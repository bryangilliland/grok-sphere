@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo First time running? Launching setup_windows.bat first...
  call setup_windows.bat
)

if not exist ".env" (
  copy ".env.example" ".env" >nul
  echo Please open companion\.env, paste your ANTHROPIC_API_KEY, save, and run run_thad.bat again.
  notepad .env
  exit /b 1
)

findstr /C:"sk-ant-YOUR_KEY_HERE" ".env" >nul 2>nul
if not errorlevel 1 (
  echo Your Anthropic API key is not filled in yet inside companion\.env.
  echo Opening companion\.env in Notepad...
  notepad .env
)

echo Starting Thad Companion (USB + Wi-Fi fallback)...
echo Leave this window open. Press Ctrl+C to stop.
".venv\Scripts\python.exe" -m thad_companion
pause
