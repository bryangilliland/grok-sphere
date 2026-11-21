@echo off
REM Serves the browser flasher at http://localhost:8000 for Chrome/Edge.
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  where python >nul 2>nul
  if errorlevel 1 (
    echo Python is not installed. Install Python 3.11+ from https://www.python.org/downloads/ first.
    pause
    exit /b 1
  )
  start "" http://localhost:8000/
  echo Leave this window open while flashing. Close it when done.
  python -m http.server 8000 --bind 127.0.0.1
  exit /b
)
start "" http://localhost:8000/
echo Leave this window open while flashing. Close it when done.
py -3 -m http.server 8000 --bind 127.0.0.1
