#include <stdlib.h>
#include <string.h>

#include "handlers.h"

typedef int (*handler_fn)(const uint8_t *in, uint32_t in_len, response_t *out);

//---------------------------------------------------------------------------------
static int handle_ping(const uint8_t *in, uint32_t in_len, response_t *out) {
  //---------------------------------------------------------------------------------
  out->type = MSG_PONG;
  if (in_len == 0)
    return PROTO_OK;

  out->data = malloc(in_len);
  if (out->data == NULL)
    return PROTO_ERR_NOMEM;

  memcpy(out->data, in, in_len); // echo the payload back
  out->len = in_len;
  return PROTO_OK;
}

// Raytracer, later. Sketch of the shape it will take:
//
// static int handle_render_tile(const uint8_t *in, uint32_t in_len,
//                               response_t *out) {
//   if (in_len < TILE_REQUEST_SIZE) return PROTO_ERR_BAD_PAYLOAD;
//   // 1. decode with get_u32() (floats: big-endian IEEE754, same as native)
//   // 2. allocate width * height * 3 bytes and trace rays into them
//   //    (use float, not double)
//   // 3. out->type = MSG_TILE_RESULT; out->data = pixels;
//   //    out->len = width * height * 3;
//   return PROTO_OK;
// }

static const struct {
  uint16_t type;
  handler_fn fn;
} handlers[] = {
    {MSG_PING, handle_ping},
    // { MSG_RENDER_TILE, handle_render_tile },
};

//---------------------------------------------------------------------------------
int dispatch_request(uint16_t type, const uint8_t *in, uint32_t in_len,
                     response_t *out) {
  //---------------------------------------------------------------------------------
  memset(out, 0, sizeof *out);

  for (size_t i = 0; i < sizeof handlers / sizeof handlers[0]; i++) {
    if (handlers[i].type == type)
      return handlers[i].fn(in, in_len, out);
  }
  return PROTO_ERR_UNKNOWN_TYPE;
}
