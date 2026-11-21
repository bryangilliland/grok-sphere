# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_standalone.bin`: flash at 0x0 (merged; erases saved Wi-Fi).
  SHA-256 6629999adf332fdcf970e054992923b1f3cef48aaf77aaf32fea9af6ae358eb9
- `bin/grok_sphere_standalone_app_0x10000.bin`: app-only update at 0x10000 (keeps settings).
  SHA-256 dddf769a29c76501349d4ecacbbde76bab5087b7926a829459b59827b113e1f1
- `previews/*.png`
Or rebuild from source with ESP-IDF 5.4.2: `cd firmware && idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
