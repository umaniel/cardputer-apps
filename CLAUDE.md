# Cardputer apps

Apps for one M5Stack Cardputer ADV, each under `apps/<name>/` as its own PlatformIO project.
App-specific notes live in `apps/<name>/CLAUDE.md`.

## Hardware: Cardputer ADV

- ESP32-S3, 8MB flash, **no PSRAM**: plan for roughly 150-200KB of free heap before your app's own
  buffers. Never hold large payloads in RAM; stream them.
- Display 240x135 (`M5Cardputer.Display`, rotation 1). Keyboard is a TCA8418 on I2C, which differs
  from the original Cardputer: use M5Cardputer >= 1.1.1. Audio is an ES8311 driven by M5Unified
  (`M5.Speaker`, `M5.Mic`).
- Known-good versions: `platform = espressif32@6.9.0`, `board = m5stack-stamps3`,
  `m5stack/M5Unified@0.2.29`, `m5stack/M5Cardputer@1.1.1`. Copy `apps/airplay/platformio.ini` for a new app.
- Japanese text: `fonts::lgfxJapanGothicP_12` costs ~110KB of flash (efontJA_12 costs ~310KB).
- lwIP holds only 6 datagrams per UDP socket; drain sockets from a task on core 1 (tcpip runs on core 0).

## How apps reach the device

- Apps are run from M5Launcher (beta build: it carries the fix that lets macOS mount the SD card in
  USB mode). The Launcher installs a `.bin` into its app partition, from the SD card or a URL.
- Every push to `main` builds all apps (GitHub Actions) and publishes one release with
  `<custom_release_name>.bin` per app. Each app is a Launcher favorite pointing at
  `https://github.com/umaniel/cardputer-apps/releases/latest/download/<Name>.bin`.
  So whatever is on `main` ships to the device: run `pio run -d apps/<name>` and make it pass first.
- New app: create `apps/<name>/` with `platformio.ini` (set `custom_release_name`) and `src/`.
  The workflow picks it up automatically and lists it in the release's `apps.json`; running the
  Sync app (`apps/sync`) on the device then adds it to the Launcher favorites. No manual
  favorite editing is needed.
- Apps that need Wi-Fi share one set of credentials in NVS namespace `airplay` (keys `ssid`,
  `pass`), entered through the setup screen in `apps/airplay` / `apps/sync`.

## Rules

- This repository is public. Never commit Wi-Fi passwords, tokens or personal data; apps ask for
  Wi-Fi credentials on the device and keep them in NVS (`Preferences`).
- Implement exactly what is asked. Suggest extra features as a question instead of adding them.

## Device quirks worth knowing

- The Launcher boot screen appears only on a real power-on (or deep-sleep wake). Enter opens the
  Launcher; any other key fast-boots the installed app.
- After using the Launcher's USB SD mode, USB serial stays gone until a real power cycle with USB
  unplugged.
- Most firmware in the Launcher's OTA catalogue targets the original Cardputer; its keyboard is dead
  on the ADV.
