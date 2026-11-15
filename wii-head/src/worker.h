//---------------------------------------------------------------------------------
// worker.h - one thread + one TCP connection per node. Pulls tiles from the
// scheduler, sends MSG_RENDER_TILE, adds the returned sums to the image.
//
// Hand-off: each worker keeps up to `pipeline` requests queued on the node, so
// the node starts the next tile as soon as it has sent the previous result
// instead of idling for a network round trip. If the connection fails, every
// tile still queued goes back to the scheduler and the worker reconnects (with
// backoff) until it has failed WORKER_MAX_FAILS times in a row without
// finishing a tile.
//---------------------------------------------------------------------------------
#ifndef WORKER_H
#define WORKER_H

#include <stdint.h>
#include <gccore.h>

#define WORKER_MAX_PIXELS	16384		// must match RENDER_MAX_PIXELS on the node
#define WORKER_PIPE_MAX		4			// upper limit for worker_t.pipeline
#define WORKER_MAX_FAILS	5			// consecutive failures before a node is given up
#define WORKER_ERR_LEN		48

typedef struct {
	uint32_t w, h;						// full image size
	uint32_t spp, depth;				// per request
	float   *sum;						// w*h*3 floats, raw sums (guarded by lock)
	uint32_t *cnt;	
	mutex_t  lock;
} image_t;

typedef enum { W_CONNECTING, W_RENDERING, W_IDLE, W_BACKOFF, W_DONE, W_DEAD } worker_state_t;

typedef struct {
	// set in config.h
	const char *ip;
	uint16_t    port;
	uint32_t    tile_w, tile_h;
	uint32_t    pipeline;				// requests kept queued on the node (0 = default 2)
	int         no_f16;					// 1: ask for f32 replies (debugging only)

	// set by main before worker_start
	image_t    *img;

	// owned by the worker thread, read by the UI (tearing is harmless there)
	volatile worker_state_t state;
	volatile uint32_t tiles;			// tiles completed
	volatile uint32_t in_flight;		// requests currently queued on the node
	volatile uint32_t cur_x, cur_y, cur_w, cur_h;	// tile the node is working on now
	volatile uint32_t px_done;			// pixel-requests completed (x img->spp = samples)
	volatile uint32_t node_ms_total;	// sum of node_ms over all tiles
	volatile uint32_t node_ms;			// last tile: node became free -> reply header (render + latency)
	volatile uint32_t xfer_ms;			// last tile: reply header -> last byte
	volatile uint32_t kbps;				// smoothed reply throughput, KB/s
	volatile uint32_t connects;			// successful connections
	volatile uint32_t failures;			// dropped connections / failed connects
	volatile uint32_t fail_streak;
	volatile uint32_t f16;				// 1 if the node answered with half floats
	volatile uint32_t last_rx_ms;		// time of the last byte received (for "quiet")
	volatile uint32_t conn_start_ms;	// when the current connect() started
	volatile int      last_error;		// errno / node status of the last failure
	char              err[WORKER_ERR_LEN];	// last failure, human readable
	lwp_t             thread;
} worker_t;

int worker_start(worker_t *w);			// 0 on success; on failure w->state == W_DEAD

uint32_t worker_now_ms(void);			// clock used for last_rx_ms

#endif
