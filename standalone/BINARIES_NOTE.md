# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_standalone.bin`: flash at 0x0 (merged; erases saved Wi-Fi).
  SHA-256 9488a6032b0a6c8d4ad82023bc32bbf1e4553d37fcd5499c789d405e55f8edb0
- `bin/grok_sphere_standalone_app_0x10000.bin`: app-only update at 0x10000 (keeps settings).
  SHA-256 8e03d8187f542f2afff385110afa2093cbf15d223e172bd80cf98180bfd409ce
- `previews/*.png`
Or rebuild from source with ESP-IDF 5.4.2: `cd firmware && idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
