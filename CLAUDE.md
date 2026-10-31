# Cardputer AirPlay

AirPlay 1 (RAOP) receiver for the M5Stack Cardputer ADV (ESP32-S3, 8MB flash, **no PSRAM**).
An iPhone streams to "Cardputer"; the device shows artwork and track info and plays through its speaker.

## Build and release

- Build: `pio run` (PlatformIO, Arduino framework, env `cardputer-adv`).
- Every push to `main` makes GitHub Actions build and publish a release with `AirPlay.bin`.
  The Cardputer installs it from the Launcher (M5Launcher) favorite pointing at
  `releases/latest/download/AirPlay.bin`, so a broken build on `main` reaches the device. Make sure
  `pio run` succeeds before pushing to `main`.
- The app is installed by the Launcher into its own app partition; keep the binary well under 3MB.

## Files

- `src/main.cpp`: Wi-Fi setup UI, screen, keys, jitter buffer and speaker output.
- `src/raop.cpp`: RTSP/RAOP receiver, AES + ALAC decode, DMAP track info, artwork, DACP remote.
- `src/alac.c`: ALAC decoder from shairport-sync (sample counts are bounds-checked; keep that).
- `src/rsa_key.h`: the public AirPort Express key used by every AirPlay 1 receiver.

## Constraints learned the hard way

- Free heap at runtime is ~130KB. Never buffer large payloads: artwork JPEGs from iOS are ~180KB and
  are decoded straight off the socket into a sprite. Bodies over 4KB are read and dropped.
- lwIP keeps only 6 datagrams per UDP socket. The audio task runs on core 1 and drains the socket each
  wakeup; on core 0 the tcpip task overflows that queue and audio packets are lost.
- Audio is played on arrival, not at the sender's timestamp (no 2s buffer fits in RAM), so video audio
  runs ~2s early. Clock drift is absorbed by resampling chunks by a few samples, not by dropping packets.
- Lost packets are replaced by silence to keep timing.
- Serial logs `[raop]`, `[ring]`, `[art]`, `[dacp]` lines every 2s or per event; use them to diagnose.

## Keys

`;`/`.` volume, `,`/`/` previous/next track, Enter play/pause, `w` forget Wi-Fi.

## Working style

Implement exactly what is asked. Suggest extra features as a question instead of adding them.
