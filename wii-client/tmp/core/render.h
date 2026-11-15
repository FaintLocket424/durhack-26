//---------------------------------------------------------------------------------
// render.h - the path tracer, tile at a time. Pure C, no Wii / socket code.
//---------------------------------------------------------------------------------
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

// Per-platform overrides (RAM-limited targets): provide a plat_config.h
#if defined(__has_include)
#  if __has_include("plat_config.h")
#    include "plat_config.h"
#  endif
#endif

#ifndef RENDER_MAX_PIXELS
#define RENDER_MAX_PIXELS	16384		// 128x128 tile -> ~196 KB reply, under PROTO_MAX_PAYLOAD
#endif
#define RENDER_MAX_SPP		1024
#define RENDER_MAX_DEPTH	16

typedef struct {
	uint32_t x0, y0, w, h;			// tile rectangle in full-image pixels
	uint32_t full_w, full_h;		// whole image resolution
	uint32_t spp;					// samples per pixel for this job
	uint32_t seed;
	uint32_t max_depth;
} tile_job_t;

// Called after every pass (1 spp over the whole tile) with the running sums.
// accum is w*h*3 floats, spp_done passes so far. Return non-zero to abort.
typedef int (*render_progress_fn)(const tile_job_t *job, const float *accum,
                                  uint32_t spp_done, void *user);

void render_set_progress(render_progress_fn fn, void *user);

// accum must hold job->w * job->h * 3 floats, zeroed by the caller.
// Fills it with the per-pixel SUM of job->spp samples (linear RGB).
// Returns 0, or -1 if the progress callback aborted.
// Not thread-safe: uses a global RNG, call from one thread only.
int render_tile(const tile_job_t *job, float *accum);

#endif
