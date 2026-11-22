# Grok Sphere — USB Audio Edition (`v3.0.0-usb-audio`)

Turns the **Waveshare ESP32-S3-Touch-LCD-1.85C(-BOX)** into a plug-and-play **USB-C Microphone + Speaker** for Windows 11 (x64 & Windows on ARM) — designed for use with the **Claude desktop app** voice mode, Zoom, Discord, or any PC audio app.

No Wi-Fi, no API keys, no wake word, no companion app, and **no drivers to install** (uses Windows 11's built-in `usbaudio2.sys` UAC2 driver).

## What it does
- **USB Audio Class 2.0 (`Grok Sphere`)**:
  - **Microphone**: 48 kHz, 16-bit, mono (with DC/hum filter and sensible desk gain on ES7210 / ICS-43434)
  - **Speaker**: 48 kHz, 16-bit, mono (with Windows host volume and mute control on ES8311 / PCM5101)
- **Audio-reactive capsule eyes & moods**:
  - **Listening (cyan)**: eyes glow and pulse live with microphone speech energy
  - **Thinking (lavender + bouncing dots)**: brief pause after you finish speaking while waiting for a reply
  - **Speaking (white)**: eyes bounce smoothly with speaker audio playback level
  - **Idle (off-white)**: natural random blinks and glances -> **Bored** after 2 minutes -> **Sleepy (`z Z`)** after 10 minutes
  - **Offline (dim grey)**: shown when USB is unplugged or suspended
  - **Touch**: tap to wake / trigger a brief Happy or Curious reaction; touch-and-hold (0.5 s) to preview all 6 moods (`Happy`, `Curious`, `Surprised`, `Sad`, `Bored`, `Sleepy`)
  - **No chime**
- **Built-in flash recovery**:
  - Hold **BOOT** (`GPIO0`) for **3 seconds** while running to enter **USB flash mode** (`"USB flash mode"` on screen), OR
  - Hold **BOOT** while plugging the USB-C cable in.

## Hardware verification (Waveshare ESP32-S3-Touch-LCD-1.85C V1 & V2)
Per Waveshare's schematics (`ESP32-S3-Touch-LCD-1.85C-Schematic.pdf` and `ESP32-S3-Touch-LCD-1.85C_V2.pdf`), USB-C port `J2` wires `DP1/DP2` and `DN1/DN2` directly through 22 Ω resistors (`R24`, `R25`) to ESP32-S3 native USB **`GPIO19` (D-) / `GPIO20` (D+)** (with no CH343/CP2102 UART bridge on the USB-C port). TinyUSB UAC2 runs natively on that USB-C port.

## Flashing in Microsoft Edge (`esptool.spacehuhn.com`)
Because TinyUSB UAC2 owns `GPIO19/20` while running, **put the sphere into ROM download mode first** (either hold **BOOT** while plugging in USB-C, or hold **BOOT** for 3 seconds until `"USB flash mode"` appears):

1. Open **https://esptool.spacehuhn.com/** in Microsoft Edge.
2. Click **Connect** and select the ESP32-S3 **USB JTAG/serial debug unit** COM port.
3. Add **`grok_sphere_usb_audio_app_0x10000.bin`** at offset **`0x10000`** (if coming from Standalone `v2.x`, partitions are identical so NVS Wi-Fi/API keys stay untouched!).
   - Or flash **`grok_sphere_usb_audio.bin`** at **`0x0`** on a blank/erased chip (no full erase needed if coming from Standalone `2.x`).
4. Click **Program**, then unplug and replug the USB-C cable (or tap reset) to boot into USB Audio mode.

## Rolling back to Standalone `2.3.4`
1. Hold **BOOT** for 3 seconds (or hold **BOOT** while plugging in USB-C) to enter ROM download mode.
2. In **https://esptool.spacehuhn.com/**, flash `../standalone/bin/grok_sphere_standalone_app_0x10000.bin` at **`0x10000`** (your saved Wi-Fi & API keys in NVS are still intact!).
3. Unplug and replug USB-C.

## Building from source (ESP-IDF 5.4.2)
```bash
cd firmware
idf.py build
```
