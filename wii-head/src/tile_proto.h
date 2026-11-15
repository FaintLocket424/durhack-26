//---------------------------------------------------------------------------------
// tile_proto.h - payload formats for MSG_RENDER_TILE / MSG_TILE_RESULT.
// Shared by the head node and the workers (put it next to protocol.h).
//
// MSG_RENDER_TILE payload, TILE_REQUEST_SIZE (60) bytes, all big-endian:
//
//   offset  size  field
//   0       4     image_w        full image size in pixels
//   4       4     image_h
//   8       4     tile_x         tile rectangle inside the image
//   12      4     tile_y
//   16      4     tile_w
//   20      4     tile_h
//   24      12    cam_pos        3 x f32
//   36      12    cam_target     3 x f32
//   48      4     fov_deg        f32
//   52      4     scene_id       u32
//   56      4     samples        u32, samples per pixel
//
// MSG_TILE_RESULT payload: tile_w * tile_h * 3 bytes, RGB8, row-major,
// no padding. The request_id in the reply matches the request.
//---------------------------------------------------------------------------------
#ifndef TILE_PROTO_H
#define TILE_PROTO_H

#include <string.h>
#include "protocol.h"

#define TILE_REQUEST_SIZE		60
#define TILE_BYTES_PER_PIXEL	3

typedef struct {
	float		cam_pos[3];
	float		cam_target[3];
	float		fov_deg;
	uint32_t	scene_id;
	uint32_t	samples;
} render_params_t;

typedef struct {
	uint32_t		image_w, image_h;
	uint32_t		x, y, w, h;
	render_params_t	p;
} tile_request_t;

//---------------------------------------------------------------------------------
static inline void put_f32(uint8_t *p, float v) {
//---------------------------------------------------------------------------------
	uint32_t u;
	memcpy(&u, &v, sizeof u);
	put_u32(p, u);
}

//---------------------------------------------------------------------------------
static inline float get_f32(const uint8_t *p) {
//---------------------------------------------------------------------------------
	uint32_t u = get_u32(p);
	float v;
	memcpy(&v, &u, sizeof v);
	return v;
}

//---------------------------------------------------------------------------------
static inline void tile_encode_request(uint8_t out[TILE_REQUEST_SIZE], const tile_request_t *r) {
//---------------------------------------------------------------------------------
	put_u32(out + 0,  r->image_w);
	put_u32(out + 4,  r->image_h);
	put_u32(out + 8,  r->x);
	put_u32(out + 12, r->y);
	put_u32(out + 16, r->w);
	put_u32(out + 20, r->h);
	for (int i = 0; i < 3; i++) {
		put_f32(out + 24 + 4 * i, r->p.cam_pos[i]);
		put_f32(out + 36 + 4 * i, r->p.cam_target[i]);
	}
	put_f32(out + 48, r->p.fov_deg);
	put_u32(out + 52, r->p.scene_id);
	put_u32(out + 56, r->p.samples);
}

// For the worker's handle_render_tile(). Returns PROTO_OK or PROTO_ERR_BAD_PAYLOAD.
//---------------------------------------------------------------------------------
static inline int tile_decode_request(const uint8_t *in, uint32_t in_len, tile_request_t *r) {
//---------------------------------------------------------------------------------
	if (in_len < TILE_REQUEST_SIZE) return PROTO_ERR_BAD_PAYLOAD;

	r->image_w = get_u32(in + 0);
	r->image_h = get_u32(in + 4);
	r->x = get_u32(in + 8);
	r->y = get_u32(in + 12);
	r->w = get_u32(in + 16);
	r->h = get_u32(in + 20);
	for (int i = 0; i < 3; i++) {
		r->p.cam_pos[i]    = get_f32(in + 24 + 4 * i);
		r->p.cam_target[i] = get_f32(in + 36 + 4 * i);
	}
	r->p.fov_deg  = get_f32(in + 48);
	r->p.scene_id = get_u32(in + 52);
	r->p.samples  = get_u32(in + 56);

	if (r->w == 0 || r->h == 0) return PROTO_ERR_BAD_PAYLOAD;
	if (r->w > r->image_w || r->h > r->image_h) return PROTO_ERR_BAD_PAYLOAD;
	if (r->x > r->image_w - r->w || r->y > r->image_h - r->h) return PROTO_ERR_BAD_PAYLOAD;
	if ((uint64_t)r->w * r->h * TILE_BYTES_PER_PIXEL > PROTO_MAX_PAYLOAD) return PROTO_ERR_BAD_PAYLOAD;
	return PROTO_OK;
}

#endif
