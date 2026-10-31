// AirPlay receiver for M5Stack Cardputer ADV.
//
// Keys on the status screen:
//   ;  volume up     .  volume down     w  forget Wi-Fi and reconfigure
//   ,  previous track                   /  next track
//   Enter  play / pause

#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>

#include "raop.h"

static constexpr const char* AIRPLAY_NAME = "Cardputer";
static constexpr uint32_t SAMPLE_RATE = 44100;

// ---------- jitter buffer (single producer: raop audio task, single consumer: loop) ----------

static constexpr size_t RING_SIZE = 16384;      // ~0.37s
static constexpr size_t PREBUFFER = 6144;       // start playing at ~0.14s
static constexpr size_t HIGH_WATER = 14000;     // sender clock faster than ours: drop
static constexpr size_t CHUNK = 1024;

// Sender and DAC clocks differ slightly (iOS runs ~0.1-0.5% fast), so the fill level drifts.
// Each chunk is stretched or squeezed by up to MAX_ADJUST samples to steer the averaged fill back
// to TARGET_FILL; that is a pitch change of <0.4%, inaudible, unlike dropping a whole packet.
static constexpr size_t TARGET_FILL = PREBUFFER;
static constexpr int DEADBAND = 768;
static constexpr int MAX_ADJUST = 4;

static int16_t s_ring[RING_SIZE];
static volatile size_t s_head, s_tail;          // head: write, tail: read
static volatile bool s_flush_req;
static bool s_playing;
static uint32_t s_underruns;
static uint32_t s_skips;                        // HIGH_WATER corrections (352 samples each)
static int32_t s_adjusted;                      // net samples removed (+) / added (-) by resampling
static volatile uint32_t s_full_drops;          // packets rejected because the ring was full
static size_t s_fill_min = RING_SIZE, s_fill_max;

static size_t ring_fill() { return (s_head - s_tail) % (RING_SIZE * 2) ; }

static void ring_push(const int16_t* samples, size_t count) {
  size_t fill = ring_fill();
  if (fill + count > RING_SIZE) {  // full: drop the packet
    s_full_drops++;
    return;
  }
  size_t h = s_head;
  for (size_t i = 0; i < count; i++) s_ring[(h + i) % RING_SIZE] = samples[i];
  s_head = (h + count) % (RING_SIZE * 2);
}

static void ring_pop(int16_t* out, size_t count) {
  size_t t = s_tail;
  for (size_t i = 0; i < count; i++) out[i] = s_ring[(t + i) % RING_SIZE];
  s_tail = (t + count) % (RING_SIZE * 2);
}

// Consume `in` samples from the ring and linearly resample them into `count` output samples.
static void ring_pop_resampled(int16_t* out, size_t count, size_t in) {
  size_t t = s_tail;
  for (size_t i = 0; i < count; i++) {
    uint32_t pos = (uint32_t)((uint64_t)i * (in - 1) * 65536 / (count - 1));  // 16.16 fixed point
    uint32_t idx = pos >> 16, frac = pos & 0xffff;
    int32_t a = s_ring[(t + idx) % RING_SIZE];
    int32_t b = s_ring[(t + min(idx + 1, (uint32_t)in - 1)) % RING_SIZE];
    out[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
  }
  s_tail = (t + in) % (RING_SIZE * 2);
}

static void on_flush() { s_flush_req = true; }

static void pump_audio() {
  static int16_t bufs[3][CHUNK];
  static int idx;

  if (s_flush_req) {
    s_flush_req = false;
    s_tail = s_head;
    s_playing = false;
    M5.Speaker.stop(0);
  }
  size_t fill = ring_fill();
  if (!s_playing) {
    if (fill < PREBUFFER) return;
    s_playing = true;
  }
  if (fill < s_fill_min) s_fill_min = fill;
  if (fill > s_fill_max) s_fill_max = fill;
  if (fill > HIGH_WATER) {
    s_tail = (s_tail + 352) % (RING_SIZE * 2);
    s_skips++;
  }

  while (M5.Speaker.isPlaying(0) < 2) {
    if (ring_fill() < CHUNK) {
      if (M5.Speaker.isPlaying(0) == 0) {
        s_playing = false;  // ran dry: rebuild the prebuffer
        s_underruns++;
      }
      return;
    }
    // exponential moving average of the fill, so packet jitter doesn't drive the correction
    static float avg_fill = TARGET_FILL;
    avg_fill += (ring_fill() - avg_fill) * 0.05f;
    int err = (int)avg_fill - (int)TARGET_FILL;
    int adjust = 0;
    if (err > DEADBAND) adjust = min(MAX_ADJUST, 1 + (err - DEADBAND) / 2048);
    if (err < -DEADBAND) adjust = -min(MAX_ADJUST, 1 + (-err - DEADBAND) / 2048);
    size_t in = CHUNK + adjust;
    if (ring_fill() < in) in = CHUNK;
    if (in == CHUNK) {
      ring_pop(bufs[idx], CHUNK);
    } else {
      ring_pop_resampled(bufs[idx], CHUNK, in);
      s_adjusted += adjust;
    }
    M5.Speaker.playRaw(bufs[idx], CHUNK, SAMPLE_RATE, false, 1, 0);
    idx = (idx + 1) % 3;
  }
}

// ---------- volume ----------

static int s_master = 160;  // 0..255, adjusted on the device

static void apply_volume() {
  float db = raop_status().volume_db;
  float v = (db <= -30.0f) ? 0.0f : (db + 30.0f) / 30.0f;  // AirPlay sends -30..0, -144 for mute
  M5.Speaker.setVolume(s_master);
  M5.Speaker.setChannelVolume(0, (uint8_t)(v * v * 255));
}

// ---------- display ----------

static auto& D = M5Cardputer.Display;

static void draw_header(const char* title) {
  D.fillScreen(TFT_BLACK);
  D.setTextColor(TFT_WHITE, TFT_BLACK);
  D.setTextSize(2);
  D.setCursor(4, 4);
  D.print(title);
  D.setTextSize(1);
}

// ---------- Wi-Fi setup ----------

static Preferences prefs;

static String read_line(const char* prompt, const String& initial, bool secret) {
  String s = initial;
  bool dirty = true;
  for (;;) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      auto st = M5Cardputer.Keyboard.keysState();
      for (char ch : st.word) s += ch;
      if (st.del && s.length()) s.remove(s.length() - 1);
      if (st.enter) return s;
      dirty = true;
    }
    if (dirty) {
      dirty = false;
      D.fillRect(0, 60, D.width(), 75, TFT_BLACK);
      D.setCursor(4, 60);
      D.println(prompt);
      D.setCursor(4, 80);
      D.setTextColor(TFT_YELLOW, TFT_BLACK);
      if (secret) {
        for (size_t i = 0; i + 1 < s.length(); i++) D.print('*');
        if (s.length()) D.print(s[s.length() - 1]);
      } else {
        D.print(s);
      }
      D.print('_');
      D.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    delay(10);
  }
}

static String pick_network() {
  draw_header("Wi-Fi setup");
  D.setCursor(4, 30);
  D.print("Scanning...");
  int n = WiFi.scanNetworks();
  if (n <= 0) return read_line("No networks found. Type SSID, Enter:", "", false);

  int sel = 0;
  bool dirty = true;
  for (;;) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      auto st = M5Cardputer.Keyboard.keysState();
      for (char ch : st.word) {
        if (ch == ';') sel = (sel + n - 1) % n;
        if (ch == '.') sel = (sel + 1) % n;
      }
      if (st.enter) return WiFi.SSID(sel);
      dirty = true;
    }
    if (dirty) {
      dirty = false;
      draw_header("Wi-Fi setup");
      D.setCursor(4, 24);
      D.print("; up  . down  Enter select");
      int first = max(0, min(sel - 3, n - 8));
      for (int i = first; i < min(n, first + 8); i++) {
        D.setCursor(4, 38 + (i - first) * 12);
        D.setTextColor(i == sel ? TFT_BLACK : TFT_WHITE, i == sel ? TFT_CYAN : TFT_BLACK);
        D.printf("%-28.28s %4d", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
      }
      D.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    delay(10);
  }
}

static bool connect_wifi(const String& ssid, const String& pass) {
  draw_header("Connecting");
  D.setCursor(4, 30);
  D.print(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save adds large latency spikes to UDP audio
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

static void setup_wifi() {
  prefs.begin("airplay", false);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.length() && connect_wifi(ssid, pass)) return;

  for (;;) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    ssid = pick_network();
    draw_header("Wi-Fi setup");
    D.setCursor(4, 30);
    D.print(ssid);
    pass = read_line("Password, Enter:", "", true);
    if (connect_wifi(ssid, pass)) {
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      return;
    }
    draw_header("Failed");
    D.setCursor(4, 30);
    D.print("Could not connect. Try again.");
    delay(1500);
  }
}

// ---------- track info and artwork (written on the RTSP task, drawn on loop) ----------

static constexpr int ART_SIZE = 131;  // full screen height minus a 2px margin each side

static SemaphoreHandle_t s_track_lock;
static String s_title, s_artist, s_album;
static M5Canvas s_art(&M5Cardputer.Display);
static bool s_has_art;
static volatile uint32_t s_track_version;  // bumped on any change; loop redraws when it moves

static void on_metadata(const char* title, const char* artist, const char* album) {
  xSemaphoreTake(s_track_lock, portMAX_DELAY);
  s_title = title;
  s_artist = artist;
  s_album = album;
  xSemaphoreGive(s_track_lock);
  s_track_version++;
}

// Feeds the JPEG decoder straight from the RTSP socket, so the image is never held in RAM.
struct ReaderWrapper : public lgfx::DataWrapper {
  raop_reader_t rd;
  void* ctx;
  int32_t pos = 0;
  int read(uint8_t* buf, uint32_t len) override {
    int n = rd(ctx, buf, len);
    if (n > 0) pos += n;
    return n;
  }
  void skip(int32_t offset) override {
    uint8_t tmp[64];
    while (offset > 0) {
      int n = read(tmp, min(offset, (int32_t)sizeof(tmp)));
      if (n <= 0) return;
      offset -= n;
    }
  }
  bool seek(uint32_t offset) override {  // forward only: it's a socket
    if ((int32_t)offset < pos) return false;
    skip(offset - pos);
    return true;
  }
  void close() override {}
  int32_t tell() override { return pos; }
};

static void on_artwork(raop_reader_t read, void* ctx, size_t len) {
  xSemaphoreTake(s_track_lock, portMAX_DELAY);
  s_art.fillSprite(TFT_BLACK);
  s_has_art = false;
  if (len) {
    ReaderWrapper w;
    w.rd = read;
    w.ctx = ctx;
    // (x, y) is the top-left of the ART_SIZE box; the datum centres the image inside it.
    // zoom 0 = fit keeping the aspect ratio. Progressive JPEGs fail here.
    s_has_art = s_art.drawJpg(&w, 0, 0, ART_SIZE, ART_SIZE, 0, 0, 0.0f, 0.0f, middle_center);
    Serial.printf("[art] %u bytes, %s\n", (unsigned)len, s_has_art ? "ok" : "decode failed");
  }
  xSemaphoreGive(s_track_lock);
  s_track_version++;
}

// Prints UTF-8 text word-wrapped (per character, which suits Japanese) into a column.
// Returns the y below the last line drawn.
static int draw_wrapped(const String& s, int x, int y, int w, int max_lines, uint16_t color) {
  static constexpr int LINE_H = 14;
  D.setTextColor(color, TFT_BLACK);
  int lines = 0;
  size_t start = 0;
  while (start < s.length() && lines < max_lines) {
    size_t end = start, fit = start;
    while (end < s.length()) {
      size_t next = end + 1;
      while (next < s.length() && (s[next] & 0xC0) == 0x80) next++;  // stay on code point bounds
      if (D.textWidth(s.substring(start, next)) > w) break;
      fit = end = next;
    }
    if (fit == start) fit = start + 1;  // a single glyph wider than the column
    String line = s.substring(start, fit);
    if (lines == max_lines - 1 && fit < s.length()) {
      while (line.length() && D.textWidth(line + "...") > w) {
        size_t cut = line.length() - 1;
        while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;
        line.remove(cut);
      }
      line += "...";
    }
    D.setCursor(x, y + lines * LINE_H);
    D.print(line);
    lines++;
    start = fit;
  }
  return y + lines * LINE_H;
}

// ---------- status screen ----------

// Artwork fills the left at full height; track info and, at its foot, the volume gauge share the
// column to its right.
static constexpr int TEXT_X = 2 + ART_SIZE + 6;
static constexpr int GAUGE_H = 6;

static void draw_footer() {
  if (raop_status().state == RAOP_IDLE) return;  // the waiting screen has no gauge
  // volume gauge: outline plus a fill proportional to the device volume (0..255)
  const int GAUGE_W = D.width() - 2 - TEXT_X;
  int gx = TEXT_X, gy = D.height() - 2 - GAUGE_H;
  int fill = (GAUGE_W - 2) * s_master / 255;
  D.drawRect(gx, gy, GAUGE_W, GAUGE_H, TFT_DARKGREY);
  D.fillRect(gx + 1, gy + 1, fill, GAUGE_H - 2, TFT_CYAN);
  D.fillRect(gx + 1 + fill, gy + 1, GAUGE_W - 2 - fill, GAUGE_H - 2, TFT_BLACK);
}

static void draw_status() {
  const auto& st = raop_status();
  static int last_state = -1;
  static uint32_t last_version = UINT32_MAX;
  if (st.state == last_state && s_track_version == last_version) {
    draw_footer();
    return;
  }
  // track info is being updated (artwork decode can take a while): try again next tick
  if (xSemaphoreTake(s_track_lock, 0) != pdTRUE) return;
  last_state = st.state;
  last_version = s_track_version;

  D.fillScreen(TFT_BLACK);
  if (st.state == RAOP_IDLE) {
    D.setFont(&fonts::Font0);
    D.setTextSize(2);
    D.setTextColor(TFT_YELLOW, TFT_BLACK);
    D.setCursor(4, 40);
    D.print("Waiting for");
    D.setCursor(4, 60);
    D.print("iPhone");
    D.setTextSize(1);
  } else {
    if (s_has_art) {
      s_art.pushSprite(2, 2);
    } else {
      D.drawRect(2, 2, ART_SIZE, ART_SIZE, TFT_DARKGREY);
    }
    D.setFont(&fonts::lgfxJapanGothicP_12);
    int x = TEXT_X, w = D.width() - x - 2;
    int y = draw_wrapped(s_title, x, 4, w, 4, TFT_WHITE);
    y = draw_wrapped(s_artist, x, y + 4, w, 2, TFT_CYAN);
    draw_wrapped(s_album, x, y + 2, w, 2, TFT_DARKGREY);
  }
  xSemaphoreGive(s_track_lock);
  draw_footer();
}

// ---------- main ----------

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  Serial.begin(115200);
  D.setRotation(1);
  D.setFont(&fonts::Font0);

  auto spk = M5.Speaker.config();
  spk.sample_rate = SAMPLE_RATE;
  M5.Speaker.config(spk);
  M5.Speaker.begin();

  setup_wifi();
  s_track_lock = xSemaphoreCreateMutex();
  s_art.setColorDepth(16);
  s_art.createSprite(ART_SIZE, ART_SIZE);
  raop_on_metadata(on_metadata);
  raop_on_artwork(on_artwork);
  raop_begin(AIRPLAY_NAME, ring_push, on_flush);
  apply_volume();
  draw_status();
}

void loop() {
  M5Cardputer.update();
  pump_audio();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto keys = M5Cardputer.Keyboard.keysState();
    if (keys.enter) raop_remote(RAOP_REMOTE_PLAYPAUSE);
    for (char ch : keys.word) {
      if (ch == ';') s_master = min(255, s_master + 16);
      if (ch == '.') s_master = max(0, s_master - 16);
      if (ch == ',') raop_remote(RAOP_REMOTE_PREV);
      if (ch == '/') raop_remote(RAOP_REMOTE_NEXT);
      if (ch == 'w') {
        prefs.remove("ssid");
        prefs.remove("pass");
        ESP.restart();
      }
    }
  }

  static float last_db = 1;
  static int last_master = -1;
  const auto& st = raop_status();
  if (st.volume_db != last_db || s_master != last_master) {
    last_db = st.volume_db;
    last_master = s_master;
    apply_volume();
  }

  static uint32_t last_draw;
  if (millis() - last_draw > 500) {
    last_draw = millis();
    draw_status();
  }

  static uint32_t last_report;
  if (s_playing && millis() - last_report > 2000) {
    last_report = millis();
    Serial.printf("[ring] fill=%u..%u underruns=%lu skips=%lu full_drops=%lu adjusted=%ld\n",
                  (unsigned)s_fill_min, (unsigned)s_fill_max, (unsigned long)s_underruns,
                  (unsigned long)s_skips, (unsigned long)s_full_drops, (long)s_adjusted);
    s_fill_min = RING_SIZE;
    s_fill_max = 0;
  }
  delay(2);
}
