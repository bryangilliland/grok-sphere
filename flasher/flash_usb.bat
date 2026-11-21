@echo off
REM Command-line fallback flasher using esptool inside the companion venv (or global python).
cd /d "%~dp0"
set "PY=..\companion\.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=py -3"

echo Looking for ESP32-S3 COM port (plug in while holding BOOT if needed)...
%PY% -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 parts\bootloader.bin 0x8000 parts\partition-table.bin 0x10000 parts\grok_sphere.bin
if errorlevel 1 (
  echo.
  echo Flash failed. Hold BOOT while plugging in the USB cable (for 2 seconds), then run again.
) else (
  echo.
  echo Done! Unplug and replug the sphere if it did not reboot automatically.
)
pause
