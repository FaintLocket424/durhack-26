//---------------------------------------------------------------------------------
// config.h - everything you edit lives here. Included by main.c only.
//---------------------------------------------------------------------------------
#ifndef CONFIG_H
#define CONFIG_H

#include "worker.h"

#define IMG_W			320
#define IMG_H			240
#define SPP_PER_REQ		16			// samples per pixel per request
//#define PASSES			4			// total spp = SPP_PER_REQ * PASSES
#define MAX_DEPTH		6			// bounce limit

// One entry per node (Wii or 3DS). Tile size is per node (w*h <= 16384): small
// tiles for slow nodes keep them from holding the whole render hostage, big
// tiles for fast nodes cut per-request network overhead.
//
// .pipeline  requests kept queued on the node (1..4, default 2). 2 means the
//            node starts its next tile straight after sending a result instead
//            of waiting a Wi-Fi round trip. Use 1 for a very slow node so it
//            doesn't sit on two tiles at the end of the render.
// .no_f16    1 = ask for full f32 pixels (twice the data). Only for debugging;
//            nodes without half-float support fall back to f32 automatically.
static worker_t workers[] = {
	{ .ip = "10.215.147.254", .port = 5555, .tile_w = 64, .tile_h = 64, .pipeline = 2 },
	// CHECK these against the address each 3DS shows on its screen; hotspot DHCP can change them
	{ .ip = "10.215.147.67", .port = 5555, .tile_w = 16, .tile_h = 16, .pipeline = 1 },
	{ .ip = "10.215.147.175", .port = 5555, .tile_w = 16, .tile_h = 16, .pipeline = 1 },
};
#define NUM_WORKERS ((int)(sizeof workers / sizeof workers[0]))

#endif
