# Grok Sphere — Thad + Claude Edition (Windows Personal Assistant)

Personal-use firmware and Windows companion app for your **Waveshare ESP32-S3-Touch-LCD-1.85C-BOX** sphere.

* **Original two-eye look on the 360×360 round screen:** two vertical off-white capsule eyes on a black background (the look from @Texas_Twins_Dad's X post that started this project, and the same eyes used in firmware 1.0 / v2). They blink when idle, go wide cyan when listening, squint with bouncing purple dots while thinking, pulse with the speaker while talking, turn red on an error, and dim grey when the laptop app isn't connected. A Clock + Fort Worth Weather screen is one tap away.
* **Talks to Claude directly from your Windows laptop:** no Mac, no cloud server, no monthly hosting bill.
* **Works over USB _and_ Wi-Fi:** streams mic/speaker audio over USB when plugged into your laptop, and automatically falls back to your local Wi-Fi network if Windows isn't seeing the USB port.

---

## 0. After you clone this repo

Firmware `.bin` files and preview PNGs are stored as text payloads in `binary_payload/` so they fit GitHub's file API (the two firmware images are over 1 MB). After cloning, restore them once before flashing:

```bat
python restore_binaries.py
```

Or on Windows, double-click `restore_binaries.bat`. This writes the real files under `flasher\` and `previews\` and checks their SHA-256.

---

## 1. Fix Windows Not Seeing the Sphere Over USB

Before flashing for the first time, Windows needs to see the sphere's COM port once. (Firmware 1.0 cannot turn off USB—the ESP32-S3's USB-Serial-JTAG controller and ROM bootloader live in hardware.)

Try these steps in order:

1. **Use a real USB data cable directly into the PC:**
   * Use a **USB-A to USB-C data cable** (many USB-C cables are power-only and have no data wires).
   * Plug straight into a USB port on the laptop/PC itself—skip USB hubs or docks while flashing.
2. **Force ROM Download Mode (most reliable):**
   * Unplug the sphere.
   * **Hold down the BOOT button** on the sphere, plug in the USB cable, keep holding BOOT for **2 seconds**, then let go.
   * Windows should play the USB chime.
3. **Check Windows Device Manager:**
   * Press `Win + X` and click **Device Manager**.
   * Expand **Ports (COM & LPT)**. You should see **`USB JTAG/serial debug unit (COMx)`** or **`USB Serial Device (COMx)`**.
4. **If it shows a yellow `!` or appears under _Universal Serial Bus devices_ instead of _Ports_:**
   * Windows 10/11 includes the built-in `usbser.sys` COM driver automatically, unless a third-party tool bound WinUSB to Interface 0.
   * Right-click `USB JTAG/serial debug unit` -> **Uninstall device** (check *Attempt to remove the driver for this device* if shown), then unplug and replug.
   * *(Advanced, only if it still binds to WinUSB)*: open [Zadig](https://zadig.akeo.ie/), click **Options -> List All Devices**, select **`USB JTAG/serial debug unit (Interface 0)`**, set target driver to **`USB Serial (CDC)`**, and click **Replace Driver**. *(Leave Interface 2 alone; Interface 0 is the COM port.)*
5. **If nothing appears at all even while holding BOOT during plug-in with a known data cable:**
   * The issue is the case's USB-C wiring or port. If you can reach the inner board's own USB-C socket, plug straight into that for this one-time flash. Once flashed, the Windows companion app works over **Wi-Fi alone** even if USB never connects!

---

## 2. Flash the Thad Firmware (Windows Browser Flasher)

1. Make sure **Python 3.11–3.13** is installed from [python.org/downloads](https://www.python.org/downloads/) (check **"Add python.exe to PATH"** during install).
2. Open the `flasher\` folder and double-click **`serve_web_flasher.bat`**.
   * This starts a local page at `http://localhost:8000/` and opens your browser. Use **Google Chrome** or **Microsoft Edge**.
3. Click **Install**, select your sphere's COM port (`USB JTAG/serial debug unit`), and click **Connect**.
4. When asked about **Erase device**:
   * **Leave it UNCHECKED** if your sphere was already on your home Wi-Fi—this keeps your saved Wi-Fi credentials!
   * **Check it** only if you want a clean wipe (on next boot the sphere shows a QR code to set up Wi-Fi).
5. When flashing reaches 100%, unplug and replug the sphere (or tap RESET). The original two white eyes will appear!
   * *(CLI fallback: if Chrome Web Serial doesn't cooperate, run `flasher\flash_usb.bat`.)*

### Setting up Wi-Fi (if erased or changing networks)
* If no Wi-Fi is saved, the sphere starts a hotspot **`GrokSphere-XXXX`** and displays a QR code.
* Scan it with your iPhone, pick your **2.4 GHz** home Wi-Fi, enter the password, and tap **Save and restart**. (Leave "Windows PC address" blank—the sphere finds your laptop automatically over local mDNS.)
* Need to wipe Wi-Fi later? Hold **BOOT for 5 seconds** and release when the screen says *"Release to reset Wi-Fi"*. (Holding **BOOT for 10 seconds** puts the sphere into USB Flash Mode.)

---

## 3. Get Your Anthropic API Key & Put It in `companion\.env`

1. Go to [console.anthropic.com](https://console.anthropic.com/) and sign in.
2. Go to **Billing** and add a small prepaid balance (e.g. **$5**—for casual use this lasts many months).
3. Go to **API Keys** -> **Create Key**, name it `Thad Sphere`, and copy the key (`sk-ant-...`).
4. In `companion\`, copy `.env.example` to **`.env`** (or run `setup_windows.bat`, which creates `.env` automatically), open **`.env`** in Notepad, and replace the placeholder with your key:
   ```
   ANTHROPIC_API_KEY=sk-ant-your-real-key-here
   ```
   Save and close Notepad. *(Never paste your key into chats or share `.env`.)*

---

## 4. Install & Run the Windows Companion App

1. Open `companion\` and double-click **`setup_windows.bat`** (one-time setup: creates `.venv` and installs `anthropic`, `faster-whisper`, `edge-tts`, `pyserial`, `websockets`, `zeroconf`).
2. Double-click **`run_thad.bat`** to start the companion!
   * Keep that terminal window open while using the sphere.
   * **Windows Firewall popup:** If Windows asks to allow Python on your **Private** (home) network, click **Allow**—that lets the sphere talk to your laptop over Wi-Fi if USB isn't used.
   * First question may take a few extra seconds while `faster-whisper` downloads its speech model locally; after that it stays cached.
3. *(Optional)* Want a standalone `.exe` instead of running from `.venv`? Run **`companion\build_exe.bat`** to build `dist\ThadSphereCompanion\ThadSphereCompanion.exe` (copy your `.env` next to the `.exe`).

### Changing voice, model, or settings
Edit `companion\config.yaml`:
* `claude_model`: `"claude-haiku-5-5"` (default, super fast & pennies/month) or `"claude-sonnet-5-5"` (deeper reasoning).
* `edge_voice`: `"en-US-AndrewNeural"` (default), `"en-US-BrianNeural"`, `"en-US-GuyNeural"`, etc.
* `tts_engine`: `"edge"` (free neural voice; auto-falls back to Windows offline voice) or `"pyttsx3"` (100% offline Windows voice).

---

## 5. How to Use Thad

* **Voice wake ("Hey Thad"):** Say *"Hey Thad, what's the weather today?"* or just say *"Hey Thad"*, wait for the chime (eyes turn cyan), and ask.
* **Push-to-Talk (always works):** **Touch and hold** the sphere's screen while speaking, then let go—or short-press the **BOOT** button to start/stop listening.
* **Interrupt / Stop:** **Tap** the screen once while Thad is speaking or thinking to stop him.
* **Clock & Fort Worth Weather:** **Tap** the screen while Thad is idle (or waiting) to flip between Thad's face and the Clock + Weather screen.

---

## 6. Rough Monthly Cost (Casual Personal Use)

Speech-to-text (`faster-whisper`), wake-word detection, and text-to-speech (`edge-tts` / Windows SAPI) all run **free ($0/month)**. You only pay Anthropic for Claude API tokens.

Per Anthropic's official pricing ([platform.claude.com/docs/en/about-claude/pricing](https://platform.claude.com/docs/en/about-claude/pricing) and [Anthropic List Prices effective Nov 9, 2026](https://assets.claude.com/a04aca860a54b7323e33f449706e9b43680e1346.pdf)):

| Model | Input Price | Output Price | Casual Use (~20 questions/day, ~600/mo) |
|---|---|---|---|
| **Claude Haiku 5.5** (`claude-haiku-5-5`, default) | $0.10 / 1M tokens | $0.50 / 1M tokens | **~$0.05 – $0.10 / month** |
| **Claude Sonnet 5.5** (`claude-sonnet-5-5`, optional) | $2.00 / 1M tokens | $10.00 / 1M tokens | **~$1.08 / month** |

*(Estimate assumes ~500 input tokens for the short system prompt + recent context + your question, and ~80 output tokens for a short 1–3 sentence spoken reply per question.)*
