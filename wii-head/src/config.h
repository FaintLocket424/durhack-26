//---------------------------------------------------------------------------------
// config.h - everything you edit lives here. Included by main.c only.
//---------------------------------------------------------------------------------
#ifndef CONFIG_H
#define CONFIG_H

#include "worker.h"

#define IMG_W			320
#define IMG_H			240
#define SPP_PER_REQ		4			// samples per pixel per request
#define PASSES			16			// total spp = SPP_PER_REQ * PASSES
#define MAX_DEPTH		6			// bounce limit

// One entry per Wii node. Tile size is per node (w*h <= 16384): small tiles for
// slow nodes keep them from holding the whole render hostage, big tiles for
// fast nodes cut per-request network overhead.
static worker_t workers[] = {
	{ .ip = "10.215.147.254", .port = 5555, .tile_w = 64, .tile_h = 64 },
	//{ .ip = "10.215.147.67", .port = 5555, .tile_w = 16, .tile_h = 16 },
	//{ .ip = "10.215.147.175", .port = 5555, .tile_w = 16, .tile_h = 16 },
};
#define NUM_WORKERS ((int)(sizeof workers / sizeof workers[0]))

#endif
