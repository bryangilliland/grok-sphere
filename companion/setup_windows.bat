@echo off
setlocal
cd /d "%~dp0"

echo === Grok Sphere: Thad + Claude Windows Setup ===
where py >nul 2>nul
if errorlevel 1 (
  where python >nul 2>nul
  if errorlevel 1 (
    echo Python 3.11+ was not found.
    echo Download it from https://www.python.org/downloads/ and check "Add python.exe to PATH".
    pause
    exit /b 1
  )
  set "PYCMD=python"
) else (
  set "PYCMD=py -3"
)

if not exist ".venv\Scripts\python.exe" (
  echo Creating virtual environment in .venv...
  %PYCMD% -m venv .venv
  if errorlevel 1 (
    echo Failed to create .venv.
    pause
    exit /b 1
  )
)

echo Upgrading pip and installing dependencies...
".venv\Scripts\python.exe" -m pip install --upgrade pip
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 (
  echo Package installation failed. Check your internet connection and Python version (3.11-3.13).
  pause
  exit /b 1
)

echo Installing optional openWakeWord support (best effort)...
".venv\Scripts\python.exe" -m pip install openwakeword --no-deps >nul 2>nul
".venv\Scripts\python.exe" -m pip install onnxruntime >nul 2>nul

if not exist ".env" (
  copy ".env.example" ".env" >nul
  echo.
  echo Created companion\.env from .env.example.
  echo IMPORTANT: Open companion\.env in Notepad and paste your ANTHROPIC_API_KEY!
)

echo.
echo Setup complete! Run run_thad.bat to start Thad.
pause
