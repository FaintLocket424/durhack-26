//---------------------------------------------------------------------------------
// protocol.h - wire format. Pure C, no platform dependencies.
//
// Every message, request or response, is one frame:
//
//   offset  size  field
//   0       4     magic         0x57525431 ("WRT1")
//   4       2     type          msg_type_t
//   6       2     flags         reserved, send 0
//   8       4     request_id    chosen by the requester, echoed in the reply
//   12      4     payload_len   number of bytes that follow the header
//   16      n     payload
//
// All integers are BIG-ENDIAN (network order). The Wii's PowerPC is big-endian
// as well, so on the Wii side ints/floats can be copied to and from the wire
// as they are. On a little-endian coordinator use struct.unpack('>...') or a
// numpy dtype such as '>f4'.
//---------------------------------------------------------------------------------
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include("plat_config.h")
#    include "plat_config.h"
#  endif
#endif

#define PROTO_MAGIC			0x57525431u
#define PROTO_HEADER_SIZE	16
#ifndef PROTO_MAX_PAYLOAD
#define PROTO_MAX_PAYLOAD	(1u << 20)		// 1 MiB, anything larger is rejected
#endif

typedef enum {
	MSG_PING        = 0x0001,
	MSG_PONG        = 0x0002,
	// raytracer
	MSG_RENDER_TILE = 0x0010,				// camera/scene params + tile rectangle
	MSG_TILE_RESULT = 0x0011,				// pixels for that tile
	MSG_ERROR       = 0xFFFF				// payload: u32 = -(proto_status_t)
} msg_type_t;

typedef enum {
	PROTO_OK               =  0,
	PROTO_ERR_BAD_MAGIC    = -1,
	PROTO_ERR_TOO_LARGE    = -2,
	PROTO_ERR_UNKNOWN_TYPE = -3,
	PROTO_ERR_BAD_PAYLOAD  = -4,
	PROTO_ERR_NOMEM        = -5,
	PROTO_ERR_ABORTED      = -6			// node is shutting down mid-render
} proto_status_t;

typedef struct {
	uint16_t type;
	uint16_t flags;
	uint32_t request_id;
	uint32_t payload_len;
} frame_header_t;

// What a handler produces. data is malloc()ed by the handler, freed by the caller.
typedef struct {
	uint16_t type;
	uint8_t *data;							// NULL when len == 0
	uint32_t len;
} response_t;

//---------------------------------------------------------------------------------
static inline void put_u16(uint8_t *p, uint16_t v) {
//---------------------------------------------------------------------------------
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

//---------------------------------------------------------------------------------
static inline void put_u32(uint8_t *p, uint32_t v) {
//---------------------------------------------------------------------------------
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

//---------------------------------------------------------------------------------
static inline uint16_t get_u16(const uint8_t *p) {
//---------------------------------------------------------------------------------
	return (uint16_t)((p[0] << 8) | p[1]);
}

//---------------------------------------------------------------------------------
static inline uint32_t get_u32(const uint8_t *p) {
//---------------------------------------------------------------------------------
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

//---------------------------------------------------------------------------------
static inline void proto_encode_header(uint8_t out[PROTO_HEADER_SIZE], const frame_header_t *h) {
//---------------------------------------------------------------------------------
	put_u32(out + 0,  PROTO_MAGIC);
	put_u16(out + 4,  h->type);
	put_u16(out + 6,  h->flags);
	put_u32(out + 8,  h->request_id);
	put_u32(out + 12, h->payload_len);
}

//---------------------------------------------------------------------------------
static inline proto_status_t proto_decode_header(const uint8_t in[PROTO_HEADER_SIZE], frame_header_t *h) {
//---------------------------------------------------------------------------------
	if (get_u32(in) != PROTO_MAGIC) return PROTO_ERR_BAD_MAGIC;

	h->type        = get_u16(in + 4);
	h->flags       = get_u16(in + 6);
	h->request_id  = get_u32(in + 8);
	h->payload_len = get_u32(in + 12);

	if (h->payload_len > PROTO_MAX_PAYLOAD) return PROTO_ERR_TOO_LARGE;
	return PROTO_OK;
}


//---------------------------------------------------------------------------------
// MSG_RENDER_TILE request payload: 9 x u32, big-endian
//   x0 y0 w h          tile rectangle in full-image pixels (origin top-left)
//   full_w full_h      resolution of the whole image (defines camera / aspect)
//   spp                samples per pixel to take in this request
//   seed               RNG seed. Use a different seed for every request, or
//                      two requests for the same tile return identical noise
//   max_depth          bounce limit per path
//
// MSG_TILE_RESULT payload: 5 x u32 echo (x0 y0 w h spp), followed by w*h*3
// big-endian IEEE754 floats (R,G,B, row-major): the SUM of the spp samples per
// pixel, linear light, NOT divided and NOT gamma corrected. The coordinator
// adds sums from different requests and divides by the total sample count.
//---------------------------------------------------------------------------------
#define TILE_REQUEST_SIZE		36
#define TILE_RESPONSE_PREFIX	20

static inline void put_f32(uint8_t *p, float v) {
	uint32_t u;
	memcpy(&u, &v, sizeof u);
	put_u32(p, u);
}

static inline float get_f32(const uint8_t *p) {
	uint32_t u = get_u32(p);
	float v;
	memcpy(&v, &u, sizeof v);
	return v;
}

#endif
