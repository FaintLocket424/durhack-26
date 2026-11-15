//---------------------------------------------------------------------------------
// coordinator.h - splits the image into tiles and farms them out to workers.
//
// One thread per worker. Each thread connects to its worker, then repeatedly
// takes the next unrendered tile from a shared pool, sends MSG_RENDER_TILE,
// waits for MSG_TILE_RESULT and copies the pixels into the image. Faster
// consoles therefore automatically render more tiles. If a worker drops out,
// its tile goes back into the pool for the others.
//---------------------------------------------------------------------------------
#ifndef COORDINATOR_H
#define COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

#include "config.h"
#include "tile_proto.h"

#define TILES_X		((IMG_W + TILE_W - 1) / TILE_W)
#define TILES_Y		((IMG_H + TILE_H - 1) / TILE_H)
#define N_TILES		(TILES_X * TILES_Y)

typedef enum { TILE_PENDING, TILE_ACTIVE, TILE_DONE, TILE_FAILED } tile_state_t;

typedef enum {
	W_CONNECTING,
	W_IDLE,			// connected, no tile available right now
	W_BUSY,			// rendering a tile
	W_DEAD,			// gave up (unreachable, or no raytracer handler)
	W_DONE			// finished normally
} worker_state_t;

typedef struct {
	uint8_t state;
	uint8_t attempts;
	uint8_t owner;	// worker index while ACTIVE
} tile_t;

struct coordinator;

typedef struct {
	struct coordinator	*c;
	int					index;
	char				ip[16];
	uint16_t			port;

	volatile worker_state_t	state;
	volatile uint32_t	tiles_done;
	volatile uint32_t	errors;
	volatile int		last_error;		// errno or proto_status_t

	uint32_t			seq;			// request id counter
	lwp_t				thread;
	bool				started;
	uint8_t				buf[TILE_W * TILE_H * TILE_BYTES_PER_PIXEL];	// reply scratch
} worker_t;

typedef struct coordinator {
	render_params_t		params;
	uint8_t				pixels[IMG_W * IMG_H * 3];		// RGB8 result

	tile_t				tiles[N_TILES];
	mutex_t				lock;						// guards tiles[] and counters
	volatile uint32_t	tiles_done;
	volatile uint32_t	tiles_failed;
	volatile int		workers_alive;				// 0 = run is over
	volatile int		stop;

	uint64_t			start_ticks, end_ticks;

	worker_t			workers[MAX_WORKERS];
	int					n_workers;
} coordinator_t;

//---------------------------------------------------------------------------------
static inline void tile_rect(int idx, int *x, int *y, int *w, int *h) {
//---------------------------------------------------------------------------------
	*x = (idx % TILES_X) * TILE_W;
	*y = (idx / TILES_X) * TILE_H;
	*w = (IMG_W - *x < TILE_W) ? IMG_W - *x : TILE_W;
	*h = (IMG_H - *y < TILE_H) ? IMG_H - *y : TILE_H;
}

void			coordinator_init(coordinator_t *c, const char *const *ips, int n, uint16_t port, const render_params_t *params);
void			coordinator_start(coordinator_t *c);		// reset and begin a render
void			coordinator_join(coordinator_t *c);			// wait for the worker threads
void			coordinator_stop(coordinator_t *c);			// ask the threads to quit
bool			coordinator_done(coordinator_t *c);			// true once the run is over
tile_state_t	coordinator_tile(coordinator_t *c, int idx, int *owner);
uint32_t		coordinator_elapsed_ms(coordinator_t *c);

#endif
