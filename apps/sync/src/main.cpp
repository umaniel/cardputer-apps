// Sync: turns every app in this repository's latest release into an M5Launcher favorite.
//
// Reads the app names from apps.json in the latest release, rewrites the "favorite" list in the
// Launcher's /config.conf on the SD card (favorites from elsewhere are kept), then returns to the
// Launcher through a short deep sleep, which makes the Launcher show its boot screen.

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_private/esp_clk.h>

// Download URLs are always built from this base, never taken from the downloaded list, so a
// tampered apps.json can at worst rename entries, not point the Launcher at foreign firmware.
static constexpr const char* RELEASE_BASE =
    "https://github.com/umaniel/cardputer-apps/releases/latest/download/";
static constexpr const char* CONFIG_FILE = "/config.conf";
static constexpr const char* CONFIG_TMP = "/config.conf.tmp";

// Cardputer ADV SD slot. HSPI, because the display owns the default SPI bus.
static constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

static SPIClass s_sd_spi(HSPI);

// ---------- Wi-Fi setup (same UI and NVS keys as apps/airplay, so either app can set it up) ----------

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

static Preferences prefs;  // namespace "airplay": Wi-Fi shared with the AirPlay app

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



static void say(const String& line, uint16_t color = TFT_WHITE) {
  D.setTextColor(color, TFT_BLACK);
  D.println(line);
  Serial.println(line);
}

// Stop here showing `why`; nothing has been written.
static void fail(const String& why) {
  say(why, TFT_RED);
  say("Favorites unchanged.", TFT_DARKGREY);
  for (;;) delay(1000);
}

static bool valid_name(const String& n) {
  if (n.isEmpty() || n.length() > 32) return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  return true;
}

static bool fetch_app_names(JsonDocument& names) {
  WiFiClientSecure tls;
  tls.setInsecure();  // see RELEASE_BASE: the list can't redirect downloads, only name them
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // GitHub redirects to its asset CDN
  if (!http.begin(tls, String(RELEASE_BASE) + "apps.json")) return false;
  int code = http.GET();
  bool ok = code == HTTP_CODE_OK && !deserializeJson(names, http.getString()) &&
            names.is<JsonArray>();
  if (!ok) say("apps.json: HTTP " + String(code), TFT_RED);
  http.end();
  return ok;
}

static bool write_config(JsonDocument& config) {
  // write a copy first so a failure can't leave the Launcher without a readable config
  File out = SD.open(CONFIG_TMP, FILE_WRITE);
  if (!out) return false;
  size_t n = serializeJsonPretty(config, out);
  out.close();
  if (n == 0) return false;
  SD.remove(CONFIG_FILE);
  return SD.rename(CONFIG_TMP, CONFIG_FILE);
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  Serial.begin(115200);
  D.setRotation(1);
  D.setFont(&fonts::Font0);
  D.fillScreen(TFT_BLACK);
  D.setCursor(0, 0);
  say("Sync favorites", TFT_CYAN);

  // After the auto-return the Launcher boots this app again unless Enter is pressed on its boot
  // screen; stop there instead of syncing and restarting in a loop. The RTC clock keeps counting
  // through deep sleep and the Launcher's restart, and restarts from zero on a power cycle.
  Preferences state;
  state.begin("sync", false);
  uint64_t returned_at = state.getULong64("returned_at", UINT64_MAX), now = esp_clk_rtc_time();
  if (now >= returned_at && now - returned_at < 60ULL * 1000 * 1000) {
    state.remove("returned_at");
    say("Already synced.");
    say("Power-cycle and press Enter on");
    say("the Launcher screen to open it.");
    for (;;) delay(1000);
  }

  s_sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, s_sd_spi, 25000000)) fail("No SD card.");

  JsonDocument config;
  File in = SD.open(CONFIG_FILE, FILE_READ);
  if (!in) fail("No /config.conf (open the Launcher once).");
  DeserializationError err = deserializeJson(config, in);
  in.close();
  if (err || !config[0].is<JsonObject>()) fail("Can't parse /config.conf.");
  JsonObject root = config[0];

  // The Launcher keeps its Wi-Fi passwords encrypted with a key only its builds know, so use ours.
  setup_wifi();
  D.fillScreen(TFT_BLACK);
  D.setCursor(0, 0);
  D.setTextSize(1);
  say("Sync favorites", TFT_CYAN);

  JsonDocument names;
  if (!fetch_app_names(names)) fail("Can't download the app list.");

  // Drop favorites that point at this repository, keep everything else, then add the current apps.
  JsonDocument merged;
  JsonArray favorites = merged.to<JsonArray>();
  String before;
  for (JsonObject fav : root["favorite"].as<JsonArray>()) {
    String link = fav["link"] | "";
    if (link.startsWith(RELEASE_BASE)) before += String(fav["name"] | "") + ",";
    else favorites.add(fav);
  }
  int count = 0;
  for (JsonVariant v : names.as<JsonArray>()) {
    String name = v | "";
    if (!valid_name(name)) continue;
    JsonObject fav = favorites.add<JsonObject>();
    fav["name"] = name;
    fav["fid"] = "";
    fav["link"] = String(RELEASE_BASE) + name + ".bin";
    say((before.indexOf(name + ",") >= 0 ? "  " : "+ ") + name,
        before.indexOf(name + ",") >= 0 ? TFT_WHITE : TFT_GREEN);
    count++;
  }
  if (count == 0) fail("The app list is empty.");
  root["favorite"] = favorites;

  if (!write_config(config)) fail("Writing /config.conf failed.");
  say(String(count) + " apps in favorites.", TFT_CYAN);

  say("Returning to the Launcher...", TFT_DARKGREY);
  delay(3000);
  state.putULong64("returned_at", esp_clk_rtc_time());
  state.end();
  // The Launcher shows its boot screen on a deep-sleep wake (as on power-on).
  esp_sleep_enable_timer_wakeup(100 * 1000);
  esp_deep_sleep_start();
}

void loop() {}
