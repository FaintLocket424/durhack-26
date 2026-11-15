// Picked up automatically by render.h / protocol.h (via __has_include).
// ESP32 has ~300 KB heap: 32x32 tile = 1024 px -> ~12 KB reply, plus 12 KB accumulator.
#define RENDER_MAX_PIXELS	1024
#define PROTO_MAX_PAYLOAD	(64u * 1024u)
