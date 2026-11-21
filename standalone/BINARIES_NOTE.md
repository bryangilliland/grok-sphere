# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_standalone.bin`: flash at 0x0 (merged; erases saved Wi-Fi).
  SHA-256 f3a05e5920f2a8701466b1bec04fe702bb1aeac722e485291f3a36dbc7dac941
- `bin/grok_sphere_standalone_app_0x10000.bin`: app-only update at 0x10000 (keeps settings).
  SHA-256 90e368b0f4f102bad14d96dea4540c43d36576a510e016cc97adb31d990c346c
- `previews/*.png`
Or rebuild from source with ESP-IDF 5.4.2: `cd firmware && idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
