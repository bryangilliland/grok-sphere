# Grok Sphere: standalone Thad (firmware 2.2.0-standalone)

The sphere talks to Claude on its own over Wi-Fi, with offline wake-word detection on the ESP32-S3. The laptop is only used to flash in Edge.
Hardware: Waveshare ESP32-S3-Touch-LCD-1.85C-BOX (16 MB flash, 8 MB PSRAM).

## Flash (once), or update without losing settings
- **Already set up? Just updating:** open https://esptool.spacehuhn.com/ in Edge, Connect, pick the sphere, add `bin/grok_sphere_standalone_app_0x10000.bin` at offset **0x10000**, Program. Saved Wi-Fi, Claude key, Deepgram key, and voice stay. Unplug and plug it back in.
- **First flash, or you want to start over:** add `bin/grok_sphere_standalone.bin` at offset **0x0**. That erases saved Wi-Fi, so the setup hotspot will appear.
- If the port won't connect, hold BOOT while you plug it in.

See `SETUP_GUIDE.txt` for the 6-step phone setup.

## How speech & wake word work
- **Hands-free wake word (offline):** runs ESP-SR WakeNet9 (`espressif/esp-sr`) directly on the sphere's mic with no cloud account. Four English wake words are bundled in the app image (no partition table change):
  - **Hi, ESP** (`wn9_hiesp`, default — trained on real voices)
  - **Alexa** (`wn9_alexa` — trained on real voices)
  - **Jarvis** (`wn9_jarvis_tts`)
  - **Computer** (`wn9_computer_tts`)
  - Toggle on/off or switch words any time on the setup page. Wake detection pauses automatically while listening, thinking, or speaking so it cannot self-trigger.
- **Speech-to-text & text-to-speech:** Anthropic's API is text+image in, text out only, so talking out loud uses a **Deepgram** key:
  - Speech to text: `POST https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true`, sending 16 kHz WAV.
  - Text to speech: `POST /v1/speak?model=aura-2-orion-en&encoding=linear16&sample_rate=16000&container=none`.
  - Deepgram gives $200 of free credit with no card (Nova-3 $0.0043/min, Aura-2 $0.030 per 1k chars).
- **Claude:** `POST https://api.anthropic.com/v1/messages` with `x-api-key` and `anthropic-version: 2026-10-21`. Default model is `claude-haiku-5-5` (`claude-sonnet-5-5` selectable). Replies are 1–3 short spoken sentences as "Thad".
- On the setup page, pick Thad's voice from 15 Aura-2 English voices (default Orion, calm US male) and tap **Preview this voice** to hear a sample on the sphere.
- Without a Deepgram key, type (or use phone dictation) in **Ask Thad** on the setup page; the answer scrolls under the eyes and shows on the page.

## Using it
- Say the wake word (default **"Hi, ESP"**), or tap the screen / press BOOT, then talk. It stops when you pause, or tap to stop early.
- Tap while it is talking to hush it.
- Hold the screen to toggle the clock and weather.
- Hold BOOT 3 to 10 s, then let go: setup hotspot. Hold 10 s: USB flash mode.
- Settings page: `http://grok-sphere.local` or the IP shown on screen. Keys are stored in NVS and only ever shown as `****last4`.
- Anyone on your home Wi-Fi can open the settings page, though keys are never revealed. The setup hotspot uses a random 8-digit WPA2 password shown on screen.
- Anthropic keys not scoped to a workspace may need `anthropic-workspace-id`; if Claude reports that, create a workspace-scoped key in console.anthropic.com.

## Layout
- `bin/`: merged bin (`0x0`) and app-only update bin (`0x10000`).
- `firmware/`: ESP-IDF 5.4.2 project (`idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`). `pack_wake_models.py` packs the 4 WakeNet9 models from `espressif/esp-sr` at build time so `partitions.csv` stays unchanged.
- `firmware/host_test/run_host_tests.sh`: 260 host checks (wake catalog, voice catalog, form parsing, setup page with key never echoed, Claude/Deepgram JSON, speech cleanup, WAV, VAD).
- `previews/`: screen states and phone setup page screenshots.
