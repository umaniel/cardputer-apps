#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimal AirPlay 1 (RAOP) receiver.
// Decoded audio is delivered as 44.1kHz mono int16 through the sink callback.

typedef void (*raop_pcm_sink_t)(const int16_t* samples, size_t count);

// Track info pushed by the sender (UTF-8). All empty = cleared (session ended).
typedef void (*raop_metadata_cb_t)(const char* title, const char* artist, const char* album);

// Blocking read of artwork bytes; returns 0 at the end of the image or on timeout.
typedef int (*raop_reader_t)(void* ctx, uint8_t* buf, int len);
// Called on the RTSP task with a JPEG of `len` bytes to be read through `read`; whatever the
// callback leaves unread is discarded afterwards. len == 0 means the artwork was cleared.
typedef void (*raop_artwork_cb_t)(raop_reader_t read, void* ctx, size_t len);

enum raop_state_t { RAOP_IDLE, RAOP_CONNECTED, RAOP_STREAMING };

struct raop_status_t {
  raop_state_t state;
  float volume_db;      // -30..0, -144 = mute
  uint32_t packets;
  uint32_t lost;
  char client[16];      // IPv4 of the sender
};

enum raop_remote_cmd_t { RAOP_REMOTE_NEXT, RAOP_REMOTE_PREV, RAOP_REMOTE_PLAYPAUSE };

void raop_begin(const char* name, raop_pcm_sink_t sink, void (*on_flush)());
void raop_on_metadata(raop_metadata_cb_t cb);
void raop_on_artwork(raop_artwork_cb_t cb);
// Ask the sender to skip tracks (DACP). Queued; no-op when nothing is streaming.
void raop_remote(raop_remote_cmd_t cmd);
const raop_status_t& raop_status();
