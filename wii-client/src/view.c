#include <math.h>
#include <gccore.h>

#include "view.h"
#include "render.h"

static mutex_t lock;
static u32 pix[RENDER_MAX_PIXELS];			// preview tile, one YUV word per pixel
static int  tw, th;
static u32  gen = 0, drawn = ~0u;
static volatile int have = 0;
static volatile int quit = 0;

//---------------------------------------------------------------------------------
static inline int clamp255(int v) {
//---------------------------------------------------------------------------------
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// One tile pixel -> one XFB word (two identical pixels: Y Cb Y Cr)
//---------------------------------------------------------------------------------
static inline u32 pack_yuv(float fr, float fg, float fb) {
//---------------------------------------------------------------------------------
	int r = clamp255((int)(fr * 255.0f));
	int g = clamp255((int)(fg * 255.0f));
	int b = clamp255((int)(fb * 255.0f));
	int y  = ((66*r + 129*g +  25*b + 128) >> 8) + 16;
	int cb = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
	int cr = ((112*r -  94*g -  18*b + 128) >> 8) + 128;
	return ((u32)y << 24) | ((u32)cb << 16) | ((u32)y << 8) | (u32)cr;
}

// Runs on the network thread after every 1-spp pass.
//---------------------------------------------------------------------------------
static int on_progress(const tile_job_t *job, const float *accum, uint32_t spp_done, void *user) {
//---------------------------------------------------------------------------------
	float inv = 1.0f / (float)spp_done;
	int n = (int)(job->w * job->h);

	LWP_MutexLock(lock);
	tw = (int)job->w;
	th = (int)job->h;
	for (int i = 0; i < n; i++)				// average -> gamma (sqrt) -> YUV
		pix[i] = pack_yuv(sqrtf(accum[i*3 + 0] * inv),
		                  sqrtf(accum[i*3 + 1] * inv),
		                  sqrtf(accum[i*3 + 2] * inv));
	gen++;
	have = 1;
	LWP_MutexUnlock(lock);

	return quit;							// non-zero aborts the render
}

//---------------------------------------------------------------------------------
void view_init(void) {
//---------------------------------------------------------------------------------
	LWP_MutexInit(&lock, false);
	render_set_progress(on_progress, NULL);
}

void view_request_quit(void) { quit = 1; }
int  view_has_tile(void)     { return have; }
void view_invalidate(void)   { drawn = ~0u; }

// Nearest-neighbour upscale of the tile to the full XFB.
//---------------------------------------------------------------------------------
int view_present(uint32_t *xfb, int W, int H) {
//---------------------------------------------------------------------------------
	static int colmap[512];
	int words = W / 2;						// u32s per XFB row
	if (words > 512) words = 512;

	LWP_MutexLock(lock);
	if (!have || gen == drawn) {
		LWP_MutexUnlock(lock);
		return 0;
	}

	for (int i = 0; i < words; i++) colmap[i] = i * tw / words;

	for (int y = 0; y < H; y++) {
		const u32 *src = pix + (y * th / H) * tw;
		u32 *dst = xfb + y * (W / 2);
		for (int i = 0; i < words; i++)
			dst[i] = src[colmap[i]];
	}
	drawn = gen;
	LWP_MutexUnlock(lock);
	return 1;
}
