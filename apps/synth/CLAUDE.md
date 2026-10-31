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

Notes use a tracker layout: `z s x d c v g b h n j m` is one octave (C to B), `q 2 w 3 e r 5 t 6 y 7 u`
the octave above, then `i 9 o 0 p`.

- Global: Tab switches PLAY/SEQ, Space starts/stops the sequence, `-`/`=` octave, `[`/`]` BPM.
- PLAY: `;`/`.` (up/down) volume, `,`/`/` (left/right) waveform.
- SEQ: arrows move the cursor (up/down track, left/right step), a note key writes the note and
  advances, Del clears the step and advances.
