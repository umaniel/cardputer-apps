# AirPlay

AirPlay 1 (RAOP) receiver. An iPhone streams to "Cardputer"; the device shows artwork and track info
and plays through its speaker. Released as `AirPlay.bin`. Build: `pio run -d apps/airplay`.

## Files

- `src/main.cpp`: Wi-Fi setup UI, screen, keys, jitter buffer and speaker output.
- `src/raop.cpp`: RTSP/RAOP receiver, AES + ALAC decode, DMAP track info, artwork, DACP remote.
- `src/alac.c`: ALAC decoder from shairport-sync (sample counts are bounds-checked; keep that).
- `src/rsa_key.h`: the public AirPort Express key used by every AirPlay 1 receiver.

## Constraints learned the hard way

- Free heap while streaming is ~130KB. Artwork JPEGs from iOS are ~180KB, so they are decoded
  straight off the RTSP socket into a sprite; other bodies over 4KB are read and dropped.
- The audio task runs on core 1 and drains the UDP socket each wakeup; on core 0 packets are lost.
- Audio is played on arrival, not at the sender's timestamp (no 2s buffer fits in RAM), so video audio
  runs ~2s early. Clock drift is absorbed by resampling chunks by a few samples, never by dropping
  packets. Lost packets are replaced by silence to keep timing.
- Serial logs `[raop]`, `[ring]`, `[art]`, `[dacp]` lines (every 2s or per event) for diagnosis.
- pyatv (`atvremote --protocol raop stream_file=...`) streams L16 PCM from a Mac, handy for testing
  without an iPhone; it exercises neither ALAC, encryption, artwork nor DACP.

## Keys

`;`/`.` volume, `,`/`/` previous/next track, Enter play/pause, `w` forget Wi-Fi.
