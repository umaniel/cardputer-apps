# Sync

Makes every app in the latest release a Launcher favorite. Released as `Sync.bin`.
Build: `pio run -d apps/sync`.

- Downloads `apps.json` (a JSON array of release names, written by the workflow) from
  `releases/latest/download/` and rewrites the `favorite` list in the Launcher's `/config.conf` on
  the SD card. Favorites whose link points at this repository are replaced; others are kept.
- Links are built from `RELEASE_BASE`, never read from the list, and names are restricted to
  `[A-Za-z0-9_-]`, so the list can't point the Launcher at other firmware.
- The config is written to a temp file and renamed over the original.
- Wi-Fi: the Launcher's stored passwords are encrypted with a key baked into its official builds, so
  Sync uses the credentials saved by the AirPlay app (NVS `airplay`) or asks for them.
- When done it deep-sleeps for 100ms so the Launcher shows its boot screen. If Enter isn't pressed
  there, the Launcher starts Sync again; Sync recognises that (RTC time within 60s of the return)
  and stops instead of looping.
- The SD card is on HSPI (SCK 40, MISO 39, MOSI 14, CS 12) because the display uses the default bus.
