// Synth: chiptune keyboard synth with a 4-track, 16-step sequencer for the Cardputer ADV.
//
// The audio task (core 1) owns every voice and the sequencer clock; the UI only sends it
// commands through a queue, so no voice state is shared between tasks.

#include <M5Cardputer.h>
#include <Preferences.h>

// ---------- constants ----------

static constexpr uint32_t SAMPLE_RATE = 32000;
static constexpr size_t CHUNK = 128;  // 4ms per buffer
static constexpr int TRACKS = 4;
static constexpr int STEPS = 16;
static constexpr int LIVE_VOICES = 6;
static constexpr int VOICES = TRACKS + LIVE_VOICES;  // 0..3 sequencer tracks, then live keys
static constexpr int32_t ENV_FULL = 1 << 16;
static constexpr int32_t AMP = 2600;  // per voice; 10 voices stay inside int16

enum Wave : uint8_t { W_PULSE50, W_PULSE25, W_TRI, W_NOISE, W_COUNT };
static const char* WAVE_NAME[W_COUNT] = {"PULSE 50%", "PULSE 25%", "TRIANGLE", "NOISE"};
static const char* TRACK_NAME[TRACKS] = {"SQ1", "SQ2", "TRI", "NOI"};
static const uint8_t TRACK_WAVE[TRACKS] = {W_PULSE50, W_PULSE25, W_TRI, W_NOISE};
static const uint16_t TRACK_COLOR[TRACKS] = {TFT_CYAN, TFT_GREENYELLOW, TFT_ORANGE, TFT_MAGENTA};
static const char* NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Tracker-style layout: bottom letter row is one octave, the q row (with digits as black keys)
// continues an octave higher.
static const char KEYMAP[] = "zsxdcvgbhnjmq2w3er5t6y7ui9o0p";
static constexpr int KEYMAP_LEN = sizeof(KEYMAP) - 1;

static int semitone_of(char c) {
  const char* p = strchr(KEYMAP, c);
  return (p && c) ? int(p - KEYMAP) : -1;
}

// ---------- shared state (UI writes, audio reads; single bytes/words only) ----------

static uint8_t s_pattern[TRACKS][STEPS];  // 0 = empty, else MIDI note
static volatile int s_bpm = 120;
static volatile int s_play_step = -1;  // written by the audio task, -1 while stopped

struct Cmd {
  enum : uint8_t { NOTE_ON, NOTE_OFF, PLAY, STOP } type;
  uint8_t slot, note, wave;
};
static QueueHandle_t s_cmds;

static void send(Cmd c) { xQueueSend(s_cmds, &c, 0); }

// ---------- audio ----------

struct Voice {
  enum : uint8_t { OFF, ATTACK, SUSTAIN, RELEASE } stage = OFF;
  uint8_t wave = W_PULSE50;
  bool percussive = false;  // decays while held (noise hits)
  uint32_t phase = 0, inc = 0, noise_inc = 0;
  uint16_t lfsr = 1;
  int32_t level = 0;
  int32_t gate = -1;  // samples until release, -1 = until NOTE_OFF
};
static Voice s_voices[VOICES];

static uint32_t note_inc(int note) {
  float hz = 440.0f * powf(2.0f, (note - 69) / 12.0f);
  return (uint32_t)(hz / SAMPLE_RATE * 4294967296.0);
}

static void voice_on(Voice& v, int note, uint8_t wave, int32_t gate) {
  v.wave = wave;
  v.inc = note_inc(note);
  // noise is clocked 32x the note frequency, at most once per sample
  uint64_t n = (uint64_t)v.inc * 32;
  v.noise_inc = n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
  v.percussive = wave == W_NOISE;
  v.gate = gate;
  v.stage = Voice::ATTACK;
}

static inline int32_t voice_sample(Voice& v) {
  switch (v.stage) {
    case Voice::OFF: return 0;
    case Voice::ATTACK:  // 1ms
      v.level += ENV_FULL / 32;
      if (v.level >= ENV_FULL) { v.level = ENV_FULL; v.stage = Voice::SUSTAIN; }
      break;
    case Voice::SUSTAIN:
      if (v.percussive) v.level -= 14;  // ~150ms decay
      break;
    case Voice::RELEASE:  // ~80ms
      v.level -= 26;
      break;
  }
  if (v.gate > 0 && --v.gate == 0) v.stage = Voice::RELEASE;
  if (v.level <= 0) { v.level = 0; v.stage = Voice::OFF; return 0; }

  int32_t s;
  switch (v.wave) {
    case W_PULSE50: s = v.phase < 0x80000000u ? AMP : -AMP; v.phase += v.inc; break;
    case W_PULSE25: s = v.phase < 0x40000000u ? AMP : -AMP; v.phase += v.inc; break;
    case W_TRI: {  // 4-bit stepped triangle
      int p = v.phase >> 27;
      int t = p < 16 ? p : 31 - p;
      s = (t * 2 - 15) * AMP / 15;
      v.phase += v.inc;
      break;
    }
    default: {  // 15-bit LFSR
      uint32_t before = v.phase;
      v.phase += v.noise_inc;
      if (v.phase < before) {
        uint16_t bit = (v.lfsr ^ (v.lfsr >> 1)) & 1;
        v.lfsr = (v.lfsr >> 1) | (bit << 14);
      }
      s = (v.lfsr & 1) ? AMP : -AMP;
      break;
    }
  }
  return s * v.level >> 16;
}

static bool s_seq_running;
static uint32_t s_step_pos;  // samples into the current step
static int s_step;

static void trigger_step(int step, uint32_t step_len) {
  for (int t = 0; t < TRACKS; t++) {
    uint8_t n = s_pattern[t][step];
    if (n) voice_on(s_voices[t], n, TRACK_WAVE[t], step_len * 3 / 4);
  }
  s_play_step = step;
}

static void apply(const Cmd& c) {
  switch (c.type) {
    case Cmd::NOTE_ON: voice_on(s_voices[TRACKS + c.slot], c.note, c.wave, -1); break;
    case Cmd::NOTE_OFF: {
      Voice& v = s_voices[TRACKS + c.slot];
      if (v.stage != Voice::OFF) v.stage = Voice::RELEASE;
      break;
    }
    case Cmd::PLAY:
      s_seq_running = true;
      s_step = 0;
      s_step_pos = 0;
      break;
    case Cmd::STOP:
      s_seq_running = false;
      s_play_step = -1;
      for (int t = 0; t < TRACKS; t++)
        if (s_voices[t].stage != Voice::OFF) s_voices[t].stage = Voice::RELEASE;
      break;
  }
}

static void render(int16_t* out) {
  uint32_t step_len = SAMPLE_RATE * 15 / s_bpm;  // sixteenth notes
  for (size_t i = 0; i < CHUNK; i++) {
    if (s_seq_running) {
      if (s_step_pos == 0) trigger_step(s_step, step_len);
      if (++s_step_pos >= step_len) {
        s_step_pos = 0;
        s_step = (s_step + 1) % STEPS;
      }
    }
    int32_t mix = 0;
    for (auto& v : s_voices) mix += voice_sample(v);
    out[i] = (int16_t)constrain(mix, -32767, 32767);
  }
}

static void audio_task(void*) {
  static int16_t bufs[3][CHUNK];
  int idx = 0;
  for (;;) {
    while (M5.Speaker.isPlaying(0) >= 2) vTaskDelay(1);
    Cmd c;
    while (xQueueReceive(s_cmds, &c, 0)) apply(c);
    render(bufs[idx]);
    M5.Speaker.playRaw(bufs[idx], CHUNK, SAMPLE_RATE, false, 1, 0);
    idx = (idx + 1) % 3;
  }
}

// ---------- settings ----------

static Preferences prefs;
static int s_volume = 128;  // 0..255
static int s_octave = 4;    // octave of the bottom row
static int s_wave = W_PULSE50;
static uint32_t s_dirty_at;  // millis() of the last unsaved change, 0 = saved

static void load_settings() {
  prefs.begin("synth");
  s_bpm = constrain(prefs.getInt("bpm", 120), 60, 240);
  s_volume = constrain(prefs.getInt("vol", 128), 0, 255);
  s_octave = constrain(prefs.getInt("oct", 4), 1, 6);
  s_wave = constrain(prefs.getInt("wave", W_PULSE50), 0, W_COUNT - 1);
  if (prefs.getBytesLength("pat") == sizeof(s_pattern)) prefs.getBytes("pat", s_pattern, sizeof(s_pattern));
}

static void save_settings() {
  prefs.putInt("bpm", s_bpm);
  prefs.putInt("vol", s_volume);
  prefs.putInt("oct", s_octave);
  prefs.putInt("wave", s_wave);
  prefs.putBytes("pat", s_pattern, sizeof(s_pattern));
  s_dirty_at = 0;
}

static void changed() { s_dirty_at = millis() | 1; }

// ---------- UI state ----------

enum Mode { MODE_PLAY, MODE_SEQ };
static Mode s_mode = MODE_PLAY;
static bool s_playing;
static int s_track, s_cursor;

struct Held {
  char key;
  int semitone;
};
static Held s_held[LIVE_VOICES];  // key 0 = slot free

static int note_for(int semitone) { return constrain(12 * (s_octave + 1) + semitone, 1, 127); }

static void note_name(char* buf, int note) { sprintf(buf, "%s%d", NOTE_NAME[note % 12], note / 12 - 1); }

// ---------- display ----------

static M5Canvas canvas(&M5Cardputer.Display);
static bool s_redraw = true;

static void draw_header() {
  auto& c = canvas;
  c.fillRect(0, 0, 240, 14, 0x2104);
  c.setTextColor(TFT_WHITE);
  c.setCursor(4, 3);
  c.print(s_mode == MODE_PLAY ? "PLAY" : "SEQ");
  c.setTextColor(s_playing ? TFT_GREEN : TFT_DARKGREY);
  c.setCursor(36, 3);
  c.print(s_playing ? "> PLAYING" : "  STOPPED");
  c.setTextColor(TFT_LIGHTGREY);
  c.setCursor(108, 3);
  c.printf("OCT%d %3dBPM VOL%2d", s_octave, (int)s_bpm, s_volume / 16);
}

static void draw_footer(const char* l1, const char* l2) {
  auto& c = canvas;
  c.setTextColor(TFT_DARKGREY);
  c.setCursor(2, 116);
  c.print(l1);
  c.setCursor(2, 126);
  c.print(l2);
}

static bool semitone_held(int s) {
  for (auto& h : s_held)
    if (h.key && h.semitone == s) return true;
  return false;
}

static void draw_play() {
  auto& c = canvas;
  c.setTextColor(TFT_YELLOW);
  c.setTextSize(2);
  c.setCursor(4, 20);
  c.print(WAVE_NAME[s_wave]);
  c.setTextSize(1);

  // piano: 17 white keys covering the 29 mapped semitones
  static const int WHITE_SEMI[] = {0, 2, 4, 5, 7, 9, 11};
  const int kw = 14, top = 44, wh = 62, bh = 36;
  int x = 1;
  for (int s = 0; s < KEYMAP_LEN; s++) {
    int pc = s % 12;
    bool white = false;
    for (int w : WHITE_SEMI) white |= (w == pc);
    if (!white) continue;
    bool on = semitone_held(s);
    c.fillRect(x, top, kw - 1, wh, on ? TFT_YELLOW : TFT_WHITE);
    c.setTextColor(TFT_DARKGREY);
    c.setCursor(x + 4, top + wh - 10);
    c.print(KEYMAP[s]);
    x += kw;
  }
  x = 1;
  for (int s = 0; s < KEYMAP_LEN; s++) {
    int pc = s % 12;
    bool white = false;
    for (int w : WHITE_SEMI) white |= (w == pc);
    if (white) {
      x += kw;
      continue;
    }
    int bx = x - 5;
    bool on = semitone_held(s);
    c.fillRect(bx, top, 9, bh, on ? TFT_ORANGE : 0x2104);
    c.setTextColor(TFT_LIGHTGREY);
    c.setCursor(bx + 2, top + bh - 10);
    c.print(KEYMAP[s]);
  }
  draw_footer("Tab:SEQ Spc:play/stop -=:octave []:BPM",
              "Up/Down:volume Left/Right:wave");
}

static void draw_seq() {
  auto& c = canvas;
  const int left = 26, cw = 13, top = 18, rh = 18;
  int play = s_play_step;
  if (play >= 0) c.fillRect(left + play * cw, top, cw, rh * TRACKS, 0x4208);
  for (int t = 0; t < TRACKS; t++) {
    int y = top + t * rh;
    c.setTextColor(t == s_track ? TFT_WHITE : TRACK_COLOR[t]);
    c.setCursor(2, y + 5);
    c.print(TRACK_NAME[t]);
    for (int s = 0; s < STEPS; s++) {
      int x = left + s * cw;
      uint8_t n = s_pattern[t][s];
      if (n) {
        c.fillRect(x + 1, y + 2, cw - 2, rh - 4, TRACK_COLOR[t]);
        c.setTextColor(TFT_BLACK);
        c.setCursor(x + 1, y + 5);
        c.print(NOTE_NAME[n % 12]);
      } else {
        c.drawFastHLine(x + 3, y + rh / 2, cw - 6, (s % 4 == 0) ? TFT_LIGHTGREY : TFT_DARKGREY);
      }
    }
  }
  c.drawRect(left + s_cursor * cw, top + s_track * rh, cw, rh, TFT_WHITE);

  char name[8] = "---";
  if (s_pattern[s_track][s_cursor]) note_name(name, s_pattern[s_track][s_cursor]);
  c.setTextColor(TRACK_COLOR[s_track]);
  c.setCursor(4, top + rh * TRACKS + 6);
  c.printf("%s step %2d: %s", TRACK_NAME[s_track], s_cursor + 1, name);
  draw_footer("Tab:PLAY Spc:play/stop -=:octave []:BPM",
              "Arrows:move key:write Del:clear");
}

static void draw() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setFont(&fonts::Font0);
  draw_header();
  if (s_mode == MODE_PLAY) draw_play();
  else draw_seq();
  canvas.pushSprite(0, 0);
}

// ---------- keys ----------

static void release_all_live() {
  for (int i = 0; i < LIVE_VOICES; i++) {
    if (s_held[i].key) send({Cmd::NOTE_OFF, (uint8_t)i, 0, 0});
    s_held[i].key = 0;
  }
}

static void key_down(char k, int semitone) {
  int slot = -1;
  for (int i = 0; i < LIVE_VOICES && slot < 0; i++)
    if (!s_held[i].key) slot = i;
  if (slot < 0) slot = 0;  // all busy: steal the first
  s_held[slot] = {k, semitone};
  int note = note_for(semitone);
  uint8_t wave = s_mode == MODE_SEQ ? TRACK_WAVE[s_track] : s_wave;
  send({Cmd::NOTE_ON, (uint8_t)slot, (uint8_t)note, wave});
  if (s_mode == MODE_SEQ) {
    s_pattern[s_track][s_cursor] = note;
    s_cursor = (s_cursor + 1) % STEPS;
    changed();
  }
}

static void key_up(char k) {
  for (int i = 0; i < LIVE_VOICES; i++) {
    if (s_held[i].key == k) {
      s_held[i].key = 0;
      send({Cmd::NOTE_OFF, (uint8_t)i, 0, 0});
    }
  }
}

static void command(char k) {
  switch (k) {
    case ' ':
      s_playing = !s_playing;
      send({s_playing ? Cmd::PLAY : Cmd::STOP, 0, 0, 0});
      return;
    case '-': if (s_octave > 1) s_octave--; break;
    case '=': if (s_octave < 6) s_octave++; break;
    case '[': s_bpm = max(60, s_bpm - 5); break;
    case ']': s_bpm = min(240, s_bpm + 5); break;
    default:
      if (s_mode == MODE_PLAY) {
        if (k == ';') s_volume = min(255, s_volume + 16);
        else if (k == '.') s_volume = max(0, s_volume - 16);
        else if (k == ',') s_wave = (s_wave + W_COUNT - 1) % W_COUNT;
        else if (k == '/') s_wave = (s_wave + 1) % W_COUNT;
        else return;
        M5.Speaker.setVolume(s_volume);
      } else {
        if (k == ';') s_track = (s_track + TRACKS - 1) % TRACKS;
        else if (k == '.') s_track = (s_track + 1) % TRACKS;
        else if (k == ',') s_cursor = (s_cursor + STEPS - 1) % STEPS;
        else if (k == '/') s_cursor = (s_cursor + 1) % STEPS;
        return;  // cursor moves are not saved
      }
  }
  changed();
}

static void handle_keys() {
  static std::vector<char> prev;
  static bool prev_tab, prev_del;
  if (!M5Cardputer.Keyboard.isChange()) return;
  auto st = M5Cardputer.Keyboard.keysState();
  std::vector<char> now;
  for (char ch : st.word) now.push_back(tolower(ch));

  for (char k : prev)
    if (std::find(now.begin(), now.end(), k) == now.end() && semitone_of(k) >= 0) key_up(k);
  for (char k : now) {
    if (std::find(prev.begin(), prev.end(), k) != prev.end()) continue;
    int semi = semitone_of(k);
    if (semi >= 0) key_down(k, semi);
    else command(k);
  }
  if (st.tab && !prev_tab) {
    release_all_live();
    s_mode = s_mode == MODE_PLAY ? MODE_SEQ : MODE_PLAY;
  }
  if (st.del && !prev_del && s_mode == MODE_SEQ) {
    s_pattern[s_track][s_cursor] = 0;
    s_cursor = (s_cursor + 1) % STEPS;
    changed();
  }
  prev = now;
  prev_tab = st.tab;
  prev_del = st.del;
  s_redraw = true;
}

// ---------- main ----------

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  canvas.setColorDepth(16);
  canvas.createSprite(240, 135);

  load_settings();

  auto spk = M5.Speaker.config();
  spk.sample_rate = SAMPLE_RATE;
  spk.dma_buf_len = 128;  // smaller DMA buffers keep key-to-sound latency around 30ms
  spk.dma_buf_count = 6;
  M5.Speaker.config(spk);
  M5.Speaker.begin();
  M5.Speaker.setVolume(s_volume);

  s_cmds = xQueueCreate(32, sizeof(Cmd));
  xTaskCreatePinnedToCore(audio_task, "synth", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
  M5Cardputer.update();
  handle_keys();

  static int last_step = -2;
  if (s_play_step != last_step) {
    last_step = s_play_step;
    if (s_mode == MODE_SEQ) s_redraw = true;
  }
  if (s_redraw) {
    s_redraw = false;
    draw();
  }
  if (s_dirty_at && millis() - s_dirty_at > 2000) save_settings();
  delay(2);
}
