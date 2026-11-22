# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_usb_audio.bin`: merged image, flash at `0x0`.
  SHA-256 1466af8d31f8131a843636ab8fc1730f273935172e5f884e812d9b857be2edbc (697,328 bytes)
- `bin/grok_sphere_usb_audio_app_0x10000.bin`: app-only image, flash at `0x10000` (preserves NVS from Standalone 2.x).
  SHA-256 49aed09f768bcc02eb970aa3d9a02e853ff3d511c760586932866edfaa4435ce (631,792 bytes)

Or rebuild from source with ESP-IDF 5.4.2:
`cd firmware && idf.py build`
