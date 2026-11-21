# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_standalone.bin`: flash at 0x0 (merged; erases saved Wi-Fi).
  SHA-256 1fa21de6d73bd8ff3573632dd55231dc09b7952407944f188a6c0517232989dc
- `bin/grok_sphere_standalone_app_0x10000.bin`: app-only update at 0x10000 (keeps settings).
  SHA-256 b769fe7d6681698ab3eb366b5450b1fea0d0686ca1fa2901b8aa2c299e63cdd1
- `previews/*.png`
Or rebuild from source with ESP-IDF 5.4.2: `cd firmware && idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
