// Minimal AirPlay 1 (RAOP) receiver for ESP32-S3 without PSRAM.
//
// Audio is played as soon as it arrives instead of at the sender's timestamp,
// so only a small jitter buffer is needed (the sender's ~2s latency is not honoured).

#include "raop.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mdns.h>

extern "C" {
#include "alac.h"
}
#include "rsa_key.h"

static constexpr uint16_t RTSP_PORT = 5000;
static constexpr uint16_t AUDIO_PORT = 6000;
static constexpr uint16_t CONTROL_PORT = 6001;
static constexpr uint16_t TIMING_PORT = 6002;

static raop_pcm_sink_t s_sink;
static void (*s_on_flush)();
static raop_status_t s_status = {RAOP_IDLE, -15.0f, 0, 0, ""};

static mbedtls_pk_context s_pk;
static uint8_t s_mac[6];

// Session state (written by the RTSP task, read by the audio task).
static SemaphoreHandle_t s_session_lock;
static mbedtls_aes_context s_aes;
static uint8_t s_aesiv[16];
static bool s_encrypted;
static bool s_pcm;  // rtpmap L16: raw big-endian PCM (pyatv); otherwise ALAC (iOS)
static alac_file* s_alac;
static volatile bool s_session_ready;
static int32_t s_last_seq = -1;

// decode timing, reported over serial every 2s
static uint32_t s_decode_us_sum, s_decode_us_max;

static int rng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

// ---------- helpers ----------

static int b64_decode(const String& in, uint8_t* out, size_t out_max) {
  String s = in;
  s.trim();
  while (s.length() % 4) s += '=';  // Apple omits the padding
  size_t olen = 0;
  if (mbedtls_base64_decode(out, out_max, &olen, (const uint8_t*)s.c_str(), s.length()) != 0)
    return -1;
  return (int)olen;
}

static String b64_encode(const uint8_t* in, size_t len) {
  size_t olen = 0;
  char buf[400];
  mbedtls_base64_encode((uint8_t*)buf, sizeof(buf), &olen, in, len);
  buf[olen] = 0;
  String s(buf);
  while (s.endsWith("=")) s.remove(s.length() - 1);
  return s;
}

static String header(const String& req, const char* name) {
  String lower = req;
  lower.toLowerCase();
  String key = String("\n") + name + ":";
  key.toLowerCase();
  int i = lower.indexOf(key);
  if (i < 0) return "";
  int start = i + key.length();
  int end = req.indexOf('\r', start);
  String v = req.substring(start, end < 0 ? req.length() : end);
  v.trim();
  return v;
}

static String apple_response(const String& challenge, IPAddress local_ip) {
  uint8_t buf[32] = {0};
  int n = b64_decode(challenge, buf, 16);
  if (n != 16) return "";
  for (int i = 0; i < 4; i++) buf[16 + i] = local_ip[i];
  memcpy(buf + 20, s_mac, 6);

  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(s_pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
  uint8_t sig[256];
  if (mbedtls_rsa_pkcs1_sign(rsa, rng, nullptr, MBEDTLS_RSA_PRIVATE, MBEDTLS_MD_NONE, 32, buf, sig) != 0)
    return "";
  return b64_encode(sig, sizeof(sig));
}

static bool decrypt_aes_key(const String& b64key, uint8_t key[16]) {
  uint8_t enc[256];
  if (b64_decode(b64key, enc, sizeof(enc)) != 256) return false;
  mbedtls_rsa_context* rsa = mbedtls_pk_rsa(s_pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
  size_t olen = 0;
  int r = mbedtls_rsa_pkcs1_decrypt(rsa, rng, nullptr, MBEDTLS_RSA_PRIVATE, &olen, enc, key, 16);
  return r == 0 && olen == 16;
}

static String sdp_attr(const String& sdp, const char* name) {
  String key = String("a=") + name + ":";
  int i = sdp.indexOf(key);
  if (i < 0) return "";
  int start = i + key.length();
  int end = sdp.indexOf('\r', start);
  if (end < 0) end = sdp.indexOf('\n', start);
  String v = sdp.substring(start, end < 0 ? sdp.length() : end);
  v.trim();
  return v;
}

static void end_session() {
  xSemaphoreTake(s_session_lock, portMAX_DELAY);
  s_session_ready = false;
  if (s_alac) {
    alac_free(s_alac);
    s_alac = nullptr;
  }
  xSemaphoreGive(s_session_lock);
  s_last_seq = -1;
  if (s_on_flush) s_on_flush();
}

// fmtp: "96 352 0 16 40 10 14 2 255 0 0 44100"
static bool setup_alac(const String& fmtp) {
  int v[12] = {0};
  int n = sscanf(fmtp.c_str(), "%d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3],
                 &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]);
  if (n != 12 || v[3] != 16 || v[7] != 2) return false;
  alac_file* a = alac_create(16, 2);
  if (!a) return false;
  a->setinfo_max_samples_per_frame = v[1];
  a->setinfo_7a = v[2];
  a->setinfo_sample_size = v[3];
  a->setinfo_rice_historymult = v[4];
  a->setinfo_rice_initialhistory = v[5];
  a->setinfo_rice_kmodifier = v[6];
  a->setinfo_7f = v[7];
  a->setinfo_80 = v[8];
  a->setinfo_82 = v[9];
  a->setinfo_86 = v[10];
  a->setinfo_8a_rate = v[11];
  alac_allocate_buffers(a);
  s_alac = a;
  return true;
}

// ---------- RTSP ----------

static bool read_headers(WiFiClient& c, String& req) {
  req = "";
  uint32_t t0 = millis();
  while (c.connected() && !req.endsWith("\r\n\r\n")) {
    if (c.available()) {
      req += (char)c.read();
      if (req.length() > 4096) return false;
    } else {
      if (millis() - t0 > 60000) return false;
      delay(2);
    }
  }
  return c.connected();
}

// Reads up to `len` body bytes, waiting for them; returns 0 on timeout or disconnect.
static int read_body(WiFiClient& c, uint8_t* buf, int len) {
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && c.connected() && millis() - t0 < 5000) {
    int n = c.read(buf + got, len - got);
    if (n > 0) {
      got += n;
      t0 = millis();
    } else {
      delay(2);
    }
  }
  return got;
}

static void skip_body(WiFiClient& c, int len) {
  uint8_t buf[512];
  while (len > 0) {
    int n = read_body(c, buf, min(len, (int)sizeof(buf)));
    if (n <= 0) return;
    len -= n;
  }
}

// Bodies we keep (SDP, volume, DMAP track info) are small. Anything bigger is read and dropped
// so it can't eat the heap.
static void read_small_body(WiFiClient& c, int len, String& body) {
  static constexpr int MAX_BODY = 4096;
  body = "";
  if (len > MAX_BODY) {
    skip_body(c, len);
    return;
  }
  body.reserve(len);
  uint8_t buf[512];
  while (len > 0) {
    int n = read_body(c, buf, min(len, (int)sizeof(buf)));
    if (n <= 0) return;
    body.concat((const char*)buf, n);
    len -= n;
  }
}

// ---------- track info and artwork ----------

static raop_metadata_cb_t s_on_metadata;
static raop_artwork_cb_t s_on_artwork;

static uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// DMAP: 4-char tag, 4-byte big-endian length, payload. Track fields sit inside an "mlit" container.
static void parse_dmap(const uint8_t* p, size_t len, String& title, String& artist, String& album) {
  while (len >= 8) {
    uint32_t n = be32(p + 4);
    if (n > len - 8) return;
    const uint8_t* v = p + 8;
    if (!memcmp(p, "mlit", 4)) parse_dmap(v, n, title, artist, album);
    else if (!memcmp(p, "minm", 4)) title = String((const char*)v, n);
    else if (!memcmp(p, "asar", 4)) artist = String((const char*)v, n);
    else if (!memcmp(p, "asal", 4)) album = String((const char*)v, n);
    p += 8 + n;
    len -= 8 + n;
  }
}

struct body_reader_t {
  WiFiClient* c;
  int remain;
};

static int artwork_read(void* ctx, uint8_t* buf, int len) {
  auto r = (body_reader_t*)ctx;
  int n = read_body(*r->c, buf, min(len, r->remain));
  r->remain -= n;
  return n;
}

static void clear_track_info() {
  if (s_on_metadata) s_on_metadata("", "", "");
  if (s_on_artwork) s_on_artwork(nullptr, nullptr, 0);
}

// ---------- remote control (DACP) ----------
//
// iOS sends "DACP-ID" and "Active-Remote" with its RTSP requests and advertises an HTTP control
// service as "iTunes_Ctrl_<DACP-ID>._dacp._tcp". Requests to it need the Active-Remote token.

static portMUX_TYPE s_dacp_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_dacp_id[24], s_active_remote[24];
static QueueHandle_t s_remote_queue;

static void remember_dacp(const String& req) {
  String id = header(req, "DACP-ID"), token = header(req, "Active-Remote");
  if (!id.length() || !token.length()) return;
  portENTER_CRITICAL(&s_dacp_mux);
  strlcpy(s_dacp_id, id.c_str(), sizeof(s_dacp_id));
  strlcpy(s_active_remote, token.c_str(), sizeof(s_active_remote));
  portEXIT_CRITICAL(&s_dacp_mux);
}

static uint16_t find_dacp_port(const char* id) {
  String want = String("iTunes_Ctrl_") + id;
  mdns_result_t* results = nullptr;
  if (mdns_query_ptr("_dacp", "_tcp", 2000, 8, &results) != ESP_OK) return 0;
  uint16_t port = 0;
  for (mdns_result_t* r = results; r; r = r->next)
    if (r->instance_name && want.equalsIgnoreCase(r->instance_name)) port = r->port;
  mdns_query_results_free(results);
  return port;
}

static void remote_task(void*) {
  String cached_id;
  uint16_t port = 0;
  for (;;) {
    uint8_t cmd;
    xQueueReceive(s_remote_queue, &cmd, portMAX_DELAY);
    char id[24], token[24];
    portENTER_CRITICAL(&s_dacp_mux);
    memcpy(id, s_dacp_id, sizeof(id));
    memcpy(token, s_active_remote, sizeof(token));
    portEXIT_CRITICAL(&s_dacp_mux);
    if (!id[0] || s_status.state == RAOP_IDLE) continue;

    if (cached_id != id || !port) {
      port = find_dacp_port(id);
      cached_id = id;
    }
    if (!port) {
      Serial.printf("[dacp] service iTunes_Ctrl_%s not found\n", id);
      continue;
    }
    const char* path = cmd == RAOP_REMOTE_NEXT   ? "nextitem"
                       : cmd == RAOP_REMOTE_PREV ? "previtem"
                                                 : "playpause";
    WiFiClient http;
    IPAddress ip;
    ip.fromString(s_status.client);
    if (!http.connect(ip, port, 2000)) {
      Serial.printf("[dacp] connect %s:%u failed\n", s_status.client, port);
      port = 0;  // the port may have changed: look it up again next time
      continue;
    }
    http.printf("GET /ctrl-int/1/%s HTTP/1.1\r\nHost: %s:%u\r\nActive-Remote: %s\r\nConnection: close\r\n\r\n",
                path, s_status.client, port, token);
    String status = http.readStringUntil('\n');
    Serial.printf("[dacp] %s -> %s\n", path, status.c_str());
    http.stop();
  }
}

void raop_remote(raop_remote_cmd_t cmd) {
  uint8_t c = cmd;
  if (s_remote_queue) xQueueSend(s_remote_queue, &c, 0);
}

static void handle_rtsp(WiFiClient& c) {
  String req, body;
  while (c.connected()) {
    if (!read_headers(c, req)) break;
    remember_dacp(req);
    int content_length = header(req, "Content-Length").toInt();
    String content_type = header(req, "Content-Type");
    if (content_type.startsWith("image/")) {
      body = "";
      if (content_type == "image/jpeg" && content_length > 0 && s_on_artwork) {
        body_reader_t r = {&c, content_length};
        s_on_artwork(artwork_read, &r, content_length);
        skip_body(c, r.remain);
      } else {
        // image/none (no artwork) or a format we can't decode
        skip_body(c, content_length);
        if (s_on_artwork) s_on_artwork(nullptr, nullptr, 0);
      }
    } else {
      read_small_body(c, content_length, body);
    }
    String method = req.substring(0, req.indexOf(' '));
    String cseq = header(req, "CSeq");
    Serial.println("[rtsp] " + method);
    String extra;

    String challenge = header(req, "Apple-Challenge");
    if (challenge.length()) {
      String r = apple_response(challenge, c.localIP());
      if (r.length()) extra += "Apple-Response: " + r + "\r\n";
    }

    if (method == "OPTIONS") {
      extra += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
    } else if (method == "ANNOUNCE") {
      end_session();
      xSemaphoreTake(s_session_lock, portMAX_DELAY);
      String rsakey = sdp_attr(body, "rsaaeskey");
      String aesiv = sdp_attr(body, "aesiv");
      s_encrypted = false;
      if (rsakey.length() && aesiv.length()) {
        uint8_t key[16];
        if (decrypt_aes_key(rsakey, key) && b64_decode(aesiv, s_aesiv, 16) == 16) {
          mbedtls_aes_setkey_dec(&s_aes, key, 128);
          s_encrypted = true;
        } else {
          Serial.println("[raop] failed to decrypt AES key");
        }
      }
      String rtpmap = sdp_attr(body, "rtpmap");
      s_pcm = rtpmap.indexOf("L16") >= 0;
      bool ok = s_pcm || setup_alac(sdp_attr(body, "fmtp"));
      xSemaphoreGive(s_session_lock);
      Serial.println("[raop] rtpmap: " + rtpmap + (s_encrypted ? " (encrypted)" : ""));
      if (!ok) Serial.println("[raop] unsupported fmtp: " + sdp_attr(body, "fmtp"));
    } else if (method == "SETUP") {
      extra += "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=" + String(AUDIO_PORT) +
               ";control_port=" + String(CONTROL_PORT) + ";timing_port=" + String(TIMING_PORT) + "\r\n";
      extra += "Session: 1\r\n";
    } else if (method == "RECORD") {
      s_last_seq = -1;
      s_session_ready = s_pcm || s_alac != nullptr;
      s_status.state = RAOP_STREAMING;
      extra += "Audio-Latency: 0\r\n";
    } else if (method == "FLUSH") {
      s_last_seq = -1;
      if (s_on_flush) s_on_flush();
    } else if (method == "TEARDOWN") {
      end_session();
      clear_track_info();
      s_status.state = RAOP_CONNECTED;
    } else if (method == "SET_PARAMETER") {
      if (content_type == "text/parameters") {
        int i = body.indexOf("volume:");
        if (i >= 0) s_status.volume_db = body.substring(i + 7).toFloat();
      } else if (content_type == "application/x-dmap-tagged" && s_on_metadata) {
        String title, artist, album;
        parse_dmap((const uint8_t*)body.c_str(), body.length(), title, artist, album);
        s_on_metadata(title.c_str(), artist.c_str(), album.c_str());
      }
    } else if (method == "GET_PARAMETER") {
      // only volume is ever requested
    }

    // e.g. "GET /info" from pyatv: answering 200 with no bplist body makes clients choke
    static const char* handled[] = {"OPTIONS", "ANNOUNCE", "SETUP", "RECORD", "FLUSH",
                                    "TEARDOWN", "SET_PARAMETER", "GET_PARAMETER"};
    bool known = false;
    for (auto m : handled) known |= method == m;
    if (!known) {
      c.print("RTSP/1.0 404 Not Found\r\nCSeq: " + cseq + "\r\nServer: AirTunes/105.1\r\n\r\n");
      continue;
    }

    String resp = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\nAudio-Jack-Status: connected; type=analog\r\nServer: AirTunes/105.1\r\n" + extra;
    if (method == "GET_PARAMETER") {
      String v = "volume: " + String(s_status.volume_db, 6) + "\r\n";
      resp += "Content-Type: text/parameters\r\nContent-Length: " + String(v.length()) + "\r\n\r\n" + v;
    } else {
      resp += "\r\n";
    }
    c.print(resp);
    if (method == "TEARDOWN") break;
  }
}

static void rtsp_task(void*) {
  WiFiServer server(RTSP_PORT);
  server.begin();
  for (;;) {
    WiFiClient c = server.available();
    if (!c) {
      delay(50);
      continue;
    }
    c.setNoDelay(true);
    strlcpy(s_status.client, c.remoteIP().toString().c_str(), sizeof(s_status.client));
    s_status.state = RAOP_CONNECTED;
    handle_rtsp(c);
    c.stop();
    end_session();
    clear_track_info();
    s_status.state = RAOP_IDLE;
    s_status.client[0] = 0;
  }
}

// ---------- audio ----------

static int udp_bind(uint16_t port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(s, (sockaddr*)&a, sizeof(a));
  return s;
}

static void decode_packet(uint8_t* p, int len) {
  if (len <= 12) return;
  int32_t seq = (p[2] << 8) | p[3];
  if (s_last_seq >= 0) {
    int16_t diff = (int16_t)(seq - s_last_seq);
    if (diff <= 0) return;  // late or duplicate
    if (diff > 1) {
      s_status.lost += diff - 1;
      // Fill short gaps with silence so playback keeps its timing; otherwise the jitter buffer
      // runs dry and stalls for a whole prebuffer.
      static const int16_t silence[352] = {0};
      if (diff <= 16 && s_sink)
        for (int i = 1; i < diff; i++) s_sink(silence, 352);
    }
  }
  s_last_seq = seq;

  static uint8_t clear[2048];
  static int16_t stereo[4096];
  static int16_t mono[2048];
  uint8_t* data = p + 12;
  int dlen = len - 12;
  if (dlen > (int)sizeof(clear)) return;

  xSemaphoreTake(s_session_lock, portMAX_DELAY);
  if (!s_session_ready || (!s_pcm && !s_alac)) {
    xSemaphoreGive(s_session_lock);
    return;
  }
  if (s_encrypted) {
    uint8_t iv[16];
    memcpy(iv, s_aesiv, 16);
    int alen = dlen & ~0xf;
    mbedtls_aes_crypt_cbc(&s_aes, MBEDTLS_AES_DECRYPT, alen, iv, data, clear);
    memcpy(clear + alen, data + alen, dlen - alen);
  } else {
    memcpy(clear, data, dlen);
  }
  int64_t t0 = esp_timer_get_time();
  int outbytes;
  if (s_pcm) {
    outbytes = min(dlen & ~3, (int)sizeof(stereo));
    for (int i = 0; i < outbytes / 2; i++) stereo[i] = (int16_t)((clear[2 * i] << 8) | clear[2 * i + 1]);
  } else {
    // the decoder's internal buffers hold max_samples_per_frame, so never let a frame claim more
    outbytes = s_alac->setinfo_max_samples_per_frame * 4;
    alac_decode_frame(s_alac, clear, stereo, &outbytes);
  }
  uint32_t us = esp_timer_get_time() - t0;
  xSemaphoreGive(s_session_lock);
  s_decode_us_sum += us;
  if (us > s_decode_us_max) s_decode_us_max = us;

  int frames = outbytes / 4;
  if (frames > 2048) frames = 2048;
  for (int i = 0; i < frames; i++) mono[i] = ((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2;
  s_status.packets++;
  if (s_sink) s_sink(mono, frames);
}

static void audio_task(void*) {
  int audio = udp_bind(AUDIO_PORT);
  int control = udp_bind(CONTROL_PORT);
  int timing = udp_bind(TIMING_PORT);
  static uint8_t pkt[2048];
  uint32_t report_at = millis() + 2000, report_pkts = 0;
  for (;;) {
    if ((int32_t)(millis() - report_at) >= 0) {
      uint32_t n = s_status.packets - report_pkts;
      Serial.printf("[raop] pkts/s=%lu decode avg=%luus max=%luus lost=%lu heap=%lu largest=%lu min=%lu\n",
                    (unsigned long)(n / 2), (unsigned long)(n ? s_decode_us_sum / n : 0),
                    (unsigned long)s_decode_us_max, (unsigned long)s_status.lost,
                    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                    (unsigned long)ESP.getMinFreeHeap());
      report_pkts = s_status.packets;
      s_decode_us_sum = s_decode_us_max = 0;
      report_at = millis() + 2000;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(audio, &fds);
    FD_SET(control, &fds);
    FD_SET(timing, &fds);
    timeval tv = {1, 0};
    int maxfd = max(audio, max(control, timing));
    int ready = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
    if (ready < 0) delay(10);  // returns immediately on error; don't spin and starve IDLE0
    if (ready <= 0) continue;

    if (FD_ISSET(audio, &fds)) {
      // drain everything queued: lwIP only holds 6 datagrams per socket
      int n;
      while ((n = recv(audio, pkt, sizeof(pkt), MSG_DONTWAIT)) > 0)
        if (n > 12 && (pkt[1] & 0x7f) == 0x60) decode_packet(pkt, n);
    }
    if (FD_ISSET(control, &fds)) {
      int n = recv(control, pkt, sizeof(pkt), 0);
      // 0x56 = retransmitted audio wrapped in a 4-byte header; 0x54 = sync (ignored)
      if (n > 16 && (pkt[1] & 0x7f) == 0x56) decode_packet(pkt + 4, n - 4);
    }
    if (FD_ISSET(timing, &fds)) {
      recv(timing, pkt, sizeof(pkt), 0);
    }
  }
}

// ---------- public ----------

void raop_begin(const char* name, raop_pcm_sink_t sink, void (*on_flush)()) {
  s_sink = sink;
  s_on_flush = on_flush;
  s_session_lock = xSemaphoreCreateMutex();
  mbedtls_aes_init(&s_aes);
  mbedtls_pk_init(&s_pk);
  if (mbedtls_pk_parse_key(&s_pk, (const uint8_t*)airport_rsa_key, sizeof(airport_rsa_key), nullptr, 0) != 0)
    Serial.println("[raop] RSA key parse failed");

  WiFi.macAddress(s_mac);
  char instance[64];
  snprintf(instance, sizeof(instance), "%02X%02X%02X%02X%02X%02X@%s", s_mac[0], s_mac[1], s_mac[2],
           s_mac[3], s_mac[4], s_mac[5], name);

  MDNS.begin("cardputer-airplay");
  MDNS.setInstanceName(instance);
  MDNS.addService("raop", "tcp", RTSP_PORT);
  const char* txt[][2] = {{"txtvers", "1"}, {"ch", "2"},     {"cn", "0,1"},   {"et", "0,1"},
                          {"sv", "false"},  {"da", "true"},  {"sr", "44100"}, {"ss", "16"},
                          {"pw", "false"},  {"vn", "3"},     {"tp", "UDP"},   {"md", "0,1"},
                          {"vs", "130.14"}, {"am", "AirPort4,107"}, {"sf", "0x4"}};
  // md=0,1: track info and artwork (artwork is streamed, never buffered whole)
  for (auto& kv : txt) MDNS.addServiceTxt("raop", "tcp", kv[0], kv[1]);

  s_remote_queue = xQueueCreate(4, sizeof(uint8_t));
  // core 1: on core 0 the higher-priority tcpip task fills the 6-slot socket queue in bursts
  // before this task gets to run, and the rest are dropped
  xTaskCreatePinnedToCore(audio_task, "raop_audio", 8192, nullptr, 5, nullptr, 1);
  // the artwork callback decodes JPEG on this task, hence the larger stack
  xTaskCreatePinnedToCore(rtsp_task, "raop_rtsp", 12288, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(remote_task, "raop_remote", 4096, nullptr, 2, nullptr, 0);
}

void raop_on_metadata(raop_metadata_cb_t cb) { s_on_metadata = cb; }
void raop_on_artwork(raop_artwork_cb_t cb) { s_on_artwork = cb; }

const raop_status_t& raop_status() { return s_status; }
