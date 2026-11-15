//---------------------------------------------------------------------------------
// sched.h - hands out tiles of different sizes without overlap.
//
// Work is PASSES sweeps over the image. In each sweep every pixel is covered
// exactly once; each caller gets the next uncovered rectangle, at most as big
// as the tile size it asked for (edges and gaps give smaller tiles). So every
// pixel ends up with exactly PASSES requests worth of samples, whatever mix of
// tile sizes the nodes use.
//---------------------------------------------------------------------------------
#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

typedef struct {
	uint32_t x0, y0, w, h;
	uint32_t seed;
} tile_t;

void sched_init(uint32_t img_w, uint32_t img_h, uint32_t passes);

// Returns  1: *t filled in, render it
//          0: nothing right now but tiles are still in flight (may be requeued), retry shortly
//         -1: all work is finished
int  sched_next(uint32_t max_w, uint32_t max_h, tile_t *t);

void sched_complete(const tile_t *t);		// result was accumulated
void sched_requeue(const tile_t *t);		// node failed, give the tile to someone else

void sched_progress(uint32_t *done, uint32_t *total);	// in pixel-requests

#endif
