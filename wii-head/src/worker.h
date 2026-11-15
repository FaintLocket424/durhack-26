//---------------------------------------------------------------------------------
// worker.h - one thread + one TCP connection per node. Pulls tiles from the
// scheduler, sends MSG_RENDER_TILE, adds the returned float sums to the image.
//---------------------------------------------------------------------------------
#ifndef WORKER_H
#define WORKER_H

#include <stdint.h>
#include <gccore.h>

#define WORKER_MAX_PIXELS	16384		// must match RENDER_MAX_PIXELS on the node

typedef struct {
	uint32_t w, h;						// full image size
	uint32_t spp, depth;				// per request
	float   *sum;						// w*h*3 floats, raw sums (guarded by lock)
	mutex_t  lock;
} image_t;

typedef enum { W_CONNECTING, W_RENDERING, W_IDLE, W_DONE, W_DEAD } worker_state_t;

typedef struct {
	// set in config.h
	const char *ip;
	uint16_t    port;
	uint32_t    tile_w, tile_h;

	// set by main before worker_start
	image_t    *img;

	// owned by the worker thread, read by the UI
	volatile worker_state_t state;
	volatile uint32_t       tiles;
	volatile int            last_error;	// >0 node status code, -1 comms/timeout, -2 connect, -3 bad tile size
	lwp_t                   thread;
} worker_t;

int worker_start(worker_t *w);			// 0 on success; on failure w->state == W_DEAD

#endif
