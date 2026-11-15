#include <math.h>
#include <3ds.h>

#include "view.h"
#include "render.h"

#define MAX_DIM 512

static LightLock lock;
static u32 pix[RENDER_MAX_PIXELS];			// preview tile, 0x00RRGGBB per pixel
static int  tw, th;
static u32  gen = 0, drawn = ~0u;
static volatile int have = 0;
static volatile int quit = 0;

//---------------------------------------------------------------------------------
static inline int clamp255(int v) {
//---------------------------------------------------------------------------------
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

//---------------------------------------------------------------------------------
static inline u32 pack_rgb(float fr, float fg, float fb) {
//---------------------------------------------------------------------------------
	u32 r = (u32)clamp255((int)(fr * 255.0f));
	u32 g = (u32)clamp255((int)(fg * 255.0f));
	u32 b = (u32)clamp255((int)(fb * 255.0f));
	return (r << 16) | (g << 8) | b;
}

// Runs on the network thread after every 1-spp pass.
//---------------------------------------------------------------------------------
static int on_progress(const tile_job_t *job, const float *accum, uint32_t spp_done, void *user) {
//---------------------------------------------------------------------------------
	(void)user;
	float inv = 1.0f / (float)spp_done;
	int n = (int)(job->w * job->h);

	LightLock_Lock(&lock);
	tw = (int)job->w;
	th = (int)job->h;
	for (int i = 0; i < n; i++)				// average -> gamma (sqrt) -> RGB
		pix[i] = pack_rgb(sqrtf(accum[i*3 + 0] * inv),
		                  sqrtf(accum[i*3 + 1] * inv),
		                  sqrtf(accum[i*3 + 2] * inv));
	gen++;
	have = 1;
	LightLock_Unlock(&lock);

	return quit;							// non-zero aborts the render
}

//---------------------------------------------------------------------------------
void view_init(void) {
//---------------------------------------------------------------------------------
	LightLock_Init(&lock);
	render_set_progress(on_progress, NULL);
}

void view_request_quit(void) { quit = 1; }
int  view_has_tile(void)     { return have; }
void view_invalidate(void)   { drawn = ~0u; }

// Nearest-neighbour upscale of the tile to the whole screen.
// 3DS framebuffers are column-major and start at the bottom-left: the pixel at
// screen (x, y) lives at byte offset (x * H + (H - 1 - y)) * 3.
//---------------------------------------------------------------------------------
int view_present(uint8_t *fb, int W, int H) {
//---------------------------------------------------------------------------------
	static int colmap[MAX_DIM], rowmap[MAX_DIM];
	if (W > MAX_DIM) W = MAX_DIM;
	if (H > MAX_DIM) H = MAX_DIM;

	LightLock_Lock(&lock);
	if (!have || gen == drawn) {
		LightLock_Unlock(&lock);
		return 0;
	}

	for (int x = 0; x < W; x++) colmap[x] = x * tw / W;
	for (int y = 0; y < H; y++) rowmap[y] = (y * th / H) * tw;

	for (int x = 0; x < W; x++) {
		uint8_t *dst = fb + x * H * 3;
		int c = colmap[x];
		for (int y = 0; y < H; y++) {
			u32 p = pix[rowmap[y] + c];
			uint8_t *d = dst + (H - 1 - y) * 3;
			d[0] = (uint8_t)p;				// B
			d[1] = (uint8_t)(p >> 8);		// G
			d[2] = (uint8_t)(p >> 16);		// R
		}
	}
	drawn = gen;
	LightLock_Unlock(&lock);
	return 1;
}
