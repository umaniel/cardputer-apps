# Synth

Chiptune keyboard synth with a 4-track, 16-step sequencer. Released as `Synth.bin`.
Build: `pio run -d apps/synth`.

## Design

- `src/main.cpp` holds everything. The audio task (core 1, priority 3) owns all voices and the
  sequencer clock; the UI talks to it only through a command queue (`Cmd`). The pattern, BPM and
  play position are single bytes/words read across tasks without locks.
- 32kHz mono, 128-sample chunks, three buffers fed to `M5.Speaker.playRaw` (same pattern as AirPlay).
  The Speaker DMA is shrunk to 6x128 samples so a key sounds within about 30ms.
- Voices: 4 sequencer voices (SQ1 pulse 50%, SQ2 pulse 25%, TRI 4-bit triangle, NOI 15-bit LFSR
  noise), plus 6 live voices for the keyboard. Sequencer notes gate for 3/4 of a step; noise decays
  on its own.
- Pattern, BPM, volume, octave and wave are saved to NVS (`synth` namespace) 2s after the last change.

## Keys

Notes use the GarageBand "Musical Typing" layout: the home row `a s d f g h j k l` is the white keys
(C up to the next D) and `w e`, `t y u`, `o p` above it are the black keys.

- Global: Tab switches PLAY/SEQ, Space starts/stops the sequence, `z`/`x` octave, `-`/`=` volume,
  `[`/`]` BPM.
- PLAY: `1`-`4` pick the waveform (kept off the frequently used keys on purpose).
- SEQ: arrows move the cursor (up/down track, left/right step), a note key writes the note and
  advances, Del clears the step and advances.
