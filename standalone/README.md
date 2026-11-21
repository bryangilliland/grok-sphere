# Grok Sphere: standalone Thad (firmware 2.0.0-standalone)

The sphere talks to Claude on its own over Wi-Fi. The laptop is only used once, to flash it in Edge.
Hardware: Waveshare ESP32-S3-Touch-LCD-1.85C-BOX (16 MB flash, 8 MB PSRAM).

## Flash (once)
- Open https://esptool.spacehuhn.com/ in Edge or Chrome, click Connect, and pick the sphere's USB port.
- Add `bin/grok_sphere_standalone.bin` at offset **0x0**, then click Program. Unplug it and plug it back in.
- The merged bin erases saved Wi-Fi, so the setup hotspot shows. That is expected.
- To update later without losing settings, flash `bin/grok_sphere_standalone_app_0x10000.bin` at **0x10000** instead.
- If the port won't connect, hold BOOT while you plug it in.

See `SETUP_GUIDE.txt` for the 6-step phone setup.

## How speech works
Anthropic's API takes text and images and returns text only. It has no speech-to-text or text-to-speech.
Voice needs a **Deepgram** key. One key covers both directions:
- Speech to text: `POST https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true`, sending 16 kHz WAV.
- Text to speech: `POST /v1/speak?model=aura-2-orion-en&encoding=linear16&sample_rate=16000&container=none`.

Deepgram gives $200 of free credit with no card. Nova-3 costs $0.0043/min and Aura-2 costs $0.030 per 1k characters. At about 20 questions a day that works out to roughly $4 a month of credit, so the free credit lasts years.
Claude: `POST https://api.anthropic.com/v1/messages` with `x-api-key` and `anthropic-version: 2026-10-21`.
- Default model is `claude-haiku-5-5`. `claude-sonnet-5-5` can be picked on the page.
- Replies are 1 to 3 short spoken sentences as "Thad".

Without a Deepgram key, type (or use phone dictation) in the **Ask Thad** box on the setup page. The answer scrolls under the eyes and also shows on the page.

## Using it
- Tap the screen or press BOOT, then talk. It stops listening when you pause, or tap to stop early.
- Tap while it is talking to hush it.
- Hold the screen to toggle the clock and weather.
- Hold BOOT 3 to 10 s, then let go: setup hotspot. Hold 10 s: USB flash mode.
- Settings page: `http://grok-sphere.local` or the IP shown on screen. Keys are stored in NVS and only ever shown as `****last4`.
- The settings page is open to anyone on your home Wi-Fi, but it never reveals the keys.
- The setup hotspot uses a random 8-digit WPA2 password, shown on screen.
- Wake word: none. Tap-to-talk only.
- Anthropic keys that are not scoped to a workspace may also need the `anthropic-workspace-id` header. If Claude reports that, make a workspace-scoped key in console.anthropic.com.

## Layout
- `bin/`: the merged bin (0x0) and the app-only bin (0x10000).
- `firmware/`: ESP-IDF 5.4.2 project. Build it with `idf.py build && idf.py merge-bin -o grok_sphere_standalone.bin`.
- `firmware/host_test/run_host_tests.sh`: 77 host checks. They cover form parsing, the setup page (key never echoed), Claude and Deepgram JSON, speech text cleanup, WAV, and VAD.
- `previews/`: screen states (`standalone_screens_preview.png`) and the phone setup page (`setup_page_first.png`, `setup_page_home.png`).
