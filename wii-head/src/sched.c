#include <stdlib.h>
#include <string.h>
#include <gccore.h>

#include "sched.h"

#define MAX_RETRY 64					// >= total tiles that can be in flight (nodes x pipeline depth)

static mutex_t  lock;
static uint8_t *cov;					// 1 = pixel already handed out in the current pass
static uint32_t W, H, passes;
static uint32_t pass, scan;				// current pass, first possibly-uncovered pixel
static uint32_t jid, in_flight, px_done;
static tile_t   retry[MAX_RETRY];
static int      nretry;
static uint32_t requeued, lost;

//---------------------------------------------------------------------------------
void sched_init(uint32_t img_w, uint32_t img_h, uint32_t n_passes) {
//---------------------------------------------------------------------------------
	W = img_w; H = img_h; passes = n_passes;
	cov = calloc(W * H, 1);
	LWP_MutexInit(&lock, false);
}

//---------------------------------------------------------------------------------
int sched_next(uint32_t mw, uint32_t mh, tile_t *t) {
//---------------------------------------------------------------------------------
	int r;
	LWP_MutexLock(lock);

	if (nretry > 0) {
		*t = retry[--nretry];
		in_flight++;
		r = 1;
	} else {
		// advance to the first uncovered pixel, starting a new pass when a sweep is full
		for (;;) {
			while (scan < W * H && cov[scan]) scan++;
			if (scan < W * H) break;
			pass++;
			memset(cov, 0, W * H);
			scan = 0;
		}

		uint32_t x = scan % W, y = scan / W, w = 1, h = 1;

		while (w < mw && x + w < W && !cov[scan + w]) w++;	// grow right

		for (int ok = 1; ok && h < mh && y + h < H; ) {		// grow down
			for (uint32_t i = 0; i < w; i++)
				if (cov[(y + h) * W + x + i]) { ok = 0; break; }
			if (ok) h++;
		}

		for (uint32_t j = 0; j < h; j++)
			memset(&cov[(y + j) * W + x], 1, w);

		t->x0 = x; t->y0 = y; t->w = w; t->h = h;
		t->seed = ++jid * 2654435761u;			// unique per request
		in_flight++;
		r = 1;
	}

	LWP_MutexUnlock(lock);
	return r;
}

//---------------------------------------------------------------------------------
void sched_complete(const tile_t *t) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	in_flight--;
	px_done += t->w * t->h;
	LWP_MutexUnlock(lock);
}

//---------------------------------------------------------------------------------
void sched_requeue(const tile_t *t) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	in_flight--;
	requeued++;
	if (nretry < MAX_RETRY) retry[nretry++] = *t;
	else lost++;
	LWP_MutexUnlock(lock);
}

//---------------------------------------------------------------------------------
void sched_progress(uint32_t *done, uint32_t *total) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	*done  = px_done;
	*total = W * H * passes;
	LWP_MutexUnlock(lock);
}

//---------------------------------------------------------------------------------
void sched_stats(sched_stats_t *st) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	st->pass      = pass;
	st->passes    = passes;
	st->in_flight = in_flight;
	st->queued    = (uint32_t)nretry;
	st->requeued  = requeued;
	st->lost      = lost;
	st->done      = px_done;
	st->total     = W * H * passes;
	LWP_MutexUnlock(lock);
}

//---------------------------------------------------------------------------------
int sched_finished(void) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(lock);
	int r = px_done >= W * H * passes && in_flight == 0 && nretry == 0;
	LWP_MutexUnlock(lock);
	return r;
}
