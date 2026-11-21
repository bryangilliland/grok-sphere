# Where are the .bin files?
GitHub copy keeps binaries as base64 text in `binary_payload/` (with SHA-256 in `manifest.json`).
Run `python restore_binaries.py` (or double-click `restore_binaries.bat`) to recreate:
- `bin/grok_sphere_standalone.bin`: flash at 0x0 (merged; erases saved Wi-Fi).
  SHA-256 a941163dc46ec5f0d3770066ba1b1ebbb36e0ae6dc7a5b58729b39b4e60d8efa
- `bin/grok_sphere_standalone_app_0x10000.bin`: app-only update at 0x10000 (keeps settings).
  SHA-256 d844b3a5ad19664c059f07d45ee79092cd628e6bade1e08e323dba31c0a203a1
- `previews/*.png`
Or rebuild from source with ESP-IDF 5.4.2: `cd firmware && idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
