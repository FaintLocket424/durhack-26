#include <stdlib.h>
#include <string.h>

#include "handlers.h"
#include "render.h"

typedef int (*handler_fn)(uint16_t flags, const uint8_t *in, uint32_t in_len, response_t *out);

//---------------------------------------------------------------------------------
static int handle_ping(uint16_t flags, const uint8_t *in, uint32_t in_len, response_t *out) {
//---------------------------------------------------------------------------------
	(void)flags;
	out->type = MSG_PONG;
	if (in_len == 0) return PROTO_OK;

	out->data = malloc(in_len);
	if (out->data == NULL) return PROTO_ERR_NOMEM;

	memcpy(out->data, in, in_len);			// echo the payload back
	out->len = in_len;
	return PROTO_OK;
}

//---------------------------------------------------------------------------------
// Request/response layout: see protocol.h
//---------------------------------------------------------------------------------
static int handle_render_tile(uint16_t flags, const uint8_t *in, uint32_t in_len, response_t *out) {
//---------------------------------------------------------------------------------
	if (in_len < TILE_REQUEST_SIZE) return PROTO_ERR_BAD_PAYLOAD;

	tile_job_t j;
	j.x0        = get_u32(in +  0);
	j.y0        = get_u32(in +  4);
	j.w         = get_u32(in +  8);
	j.h         = get_u32(in + 12);
	j.full_w    = get_u32(in + 16);
	j.full_h    = get_u32(in + 20);
	j.spp       = get_u32(in + 24);
	j.seed      = get_u32(in + 28);
	j.max_depth = get_u32(in + 32);

	// Check each field before multiplying so nothing can overflow.
	if (j.w == 0 || j.h == 0 || j.w > 4096 || j.h > 4096) return PROTO_ERR_BAD_PAYLOAD;
	if (j.w * j.h > RENDER_MAX_PIXELS)                    return PROTO_ERR_BAD_PAYLOAD;
	if (j.full_w == 0 || j.full_h == 0 || j.full_w > 16384 || j.full_h > 16384)
		return PROTO_ERR_BAD_PAYLOAD;
	if (j.x0 > j.full_w || j.w > j.full_w - j.x0)         return PROTO_ERR_BAD_PAYLOAD;
	if (j.y0 > j.full_h || j.h > j.full_h - j.y0)         return PROTO_ERR_BAD_PAYLOAD;
	if (j.spp == 0 || j.spp > RENDER_MAX_SPP)             return PROTO_ERR_BAD_PAYLOAD;
	if (j.max_depth == 0 || j.max_depth > RENDER_MAX_DEPTH) return PROTO_ERR_BAD_PAYLOAD;

	int      half  = (flags & FLAG_F16_RESULT) != 0;	// coordinator wants half floats
	uint32_t esize = half ? 2 : 4;
	uint32_t npix  = j.w * j.h;
	uint32_t nflt  = npix * 3;
	float   *accum = calloc(nflt, sizeof(float));
	uint8_t *data  = malloc(TILE_RESPONSE_PREFIX + nflt * esize);
	if (accum == NULL || data == NULL) {
		free(accum);
		free(data);
		return PROTO_ERR_NOMEM;
	}

	int rc = render_tile(&j, accum);		// blocks for the whole job
	if (rc < 0) {
		free(accum);
		free(data);
		return PROTO_ERR_ABORTED;
	}

	put_u32(data +  0, j.x0);
	put_u32(data +  4, j.y0);
	put_u32(data +  8, j.w);
	put_u32(data + 12, j.h);
	put_u32(data + 16, j.spp);
	uint8_t *px = data + TILE_RESPONSE_PREFIX;
	if (half) {
		for (uint32_t i = 0; i < nflt; i++)
			put_u16(px + i * 2, f32_to_f16(accum[i]));
	} else {
		for (uint32_t i = 0; i < nflt; i++)
			put_f32(px + i * 4, accum[i]);
	}
	free(accum);

	out->type  = MSG_TILE_RESULT;
	out->flags = half ? FLAG_F16_RESULT : 0;
	out->data  = data;
	out->len   = TILE_RESPONSE_PREFIX + nflt * esize;
	return PROTO_OK;
}

static const struct { uint16_t type; handler_fn fn; } handlers[] = {
	{ MSG_PING,        handle_ping },
	{ MSG_RENDER_TILE, handle_render_tile },
};

//---------------------------------------------------------------------------------
int dispatch_request(uint16_t type, uint16_t flags, const uint8_t *in, uint32_t in_len, response_t *out) {
//---------------------------------------------------------------------------------
	memset(out, 0, sizeof *out);

	for (size_t i = 0; i < sizeof handlers / sizeof handlers[0]; i++) {
		if (handlers[i].type == type) return handlers[i].fn(flags, in, in_len, out);
	}
	return PROTO_ERR_UNKNOWN_TYPE;
}
