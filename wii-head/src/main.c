// Wii render coordinator: splits the image into tiles, farms them out to the
// nodes in config.h, prints status until everything is back, then shows the image.
//   A    toggle between image and status
//   HOME quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/if_config.h>
#include <ogc/lwp_watchdog.h>

#include "sched.h"
#include "worker.h"
#include "config.h"

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

//---------------------------------------------------------------------------------
static void initialise(void) {
//---------------------------------------------------------------------------------
	VIDEO_Init();
	WPAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

//---------------------------------------------------------------------------------
static void wait_home(void) {
//---------------------------------------------------------------------------------
	while (SYS_MainLoop()) {
		VIDEO_WaitVSync();
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
	}
}

//---------------------------------------------------------------------------------
static const char *state_name(worker_state_t s) {
//---------------------------------------------------------------------------------
	switch (s) {
		case W_CONNECTING: return "connecting";
		case W_RENDERING:  return "rendering ";
		case W_IDLE:       return "waiting   ";
		case W_DONE:       return "done      ";
		default:           return "DEAD      ";
	}
}

//---------------------------------------------------------------------------------
static void print_status(const char *my_ip, unsigned elapsed, int complete, int finished) {
//---------------------------------------------------------------------------------
	uint32_t done, total;
	sched_progress(&done, &total);

	printf("\x1b[2;0H");
	printf("Wii render coordinator (%s)\n\n", my_ip);
	printf("image %dx%d, %d passes x %d spp, depth %d\n\n",
	       IMG_W, IMG_H, PASSES, SPP_PER_REQ, MAX_DEPTH);

	for (int i = 0; i < NUM_WORKERS; i++) {
		const worker_t *w = &workers[i];
		printf("%-15s %4ux%-4u %s %5u tiles", w->ip, (unsigned)w->tile_w, (unsigned)w->tile_h,
		       state_name(w->state), (unsigned)w->tiles);
		if (w->state == W_DEAD) printf("  err %d", w->last_error);
		printf("            \n");
	}

	printf("\nprogress: %3u%%   elapsed: %us   \n", (unsigned)((uint64_t)done * 100 / total), elapsed);
	if (finished)
		printf("%s\n\n", complete ? "complete.                            "
		                          : "INCOMPLETE: all nodes dropped out.  ");
	else
		printf("\nHOME to quit                         \n");
}

//---------------------------------------------------------------------------------
static inline int clamp255(int v) {
//---------------------------------------------------------------------------------
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

//---------------------------------------------------------------------------------
// Two screen pixels -> one XFB word (Y0 Cb Y1 Cr).
//---------------------------------------------------------------------------------
static inline uint32_t pack_yuv2(const uint8_t *a, const uint8_t *b) {
//---------------------------------------------------------------------------------
	int y0 = ((66 * a[0] + 129 * a[1] + 25 * a[2] + 128) >> 8) + 16;
	int y1 = ((66 * b[0] + 129 * b[1] + 25 * b[2] + 128) >> 8) + 16;
	int r = (a[0] + b[0]) >> 1, g = (a[1] + b[1]) >> 1, bl = (a[2] + b[2]) >> 1;
	int cb = ((-38 * r -  74 * g + 112 * bl + 128) >> 8) + 128;
	int cr = ((112 * r -  94 * g -  18 * bl + 128) >> 8) + 128;
	return ((uint32_t)y0 << 24) | ((uint32_t)cb << 16) | ((uint32_t)y1 << 8) | (uint32_t)cr;
}

// Raw sums -> average -> gamma (sqrt) -> 8-bit RGB
//---------------------------------------------------------------------------------
static void tonemap(const float *sum, uint8_t *rgb) {
//---------------------------------------------------------------------------------
	float inv = 1.0f / (float)(SPP_PER_REQ * PASSES);
	for (int i = 0; i < IMG_W * IMG_H * 3; i++)
		rgb[i] = (uint8_t)clamp255((int)(sqrtf(sum[i] * inv) * 255.0f + 0.5f));
}

// Nearest-neighbour stretch of the image over the whole XFB.
//---------------------------------------------------------------------------------
static void draw_image(const uint8_t *rgb) {
//---------------------------------------------------------------------------------
	const int W = rmode->fbWidth, H = rmode->xfbHeight;
	uint32_t *fb = (uint32_t *)xfb;

	for (int y = 0; y < H; y++) {
		const uint8_t *row = rgb + (y * IMG_H / H) * IMG_W * 3;
		uint32_t *dst = fb + y * (W / 2);
		for (int i = 0; i < W / 2; i++) {
			const uint8_t *a = row + ((2 * i)     * IMG_W / W) * 3;
			const uint8_t *b = row + ((2 * i + 1) * IMG_W / W) * 3;
			dst[i] = pack_yuv2(a, b);
		}
	}
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	initialise();

	printf("\x1b[2;0H");
	printf("Wii render coordinator\n\nconfiguring network... ");

	char my_ip[16] = {0}, netmask[16] = {0}, gateway[16] = {0};
	if (if_config(my_ip, netmask, gateway, true, 20) < 0) {
		printf("FAILED\n\nHOME to quit\n");
		wait_home();
		return 1;
	}

	static image_t img;
	img.w = IMG_W;
	img.h = IMG_H;
	img.spp = SPP_PER_REQ;
	img.depth = MAX_DEPTH;
	img.sum = calloc(IMG_W * IMG_H * 3, sizeof(float));
	uint8_t *rgb = malloc(IMG_W * IMG_H * 3);
	if (!img.sum || !rgb) {
		printf("out of memory\n");
		wait_home();
		return 1;
	}
	LWP_MutexInit(&img.lock, false);

	sched_init(IMG_W, IMG_H, PASSES);

	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
	u64 t0 = gettime();
	for (int i = 0; i < NUM_WORKERS; i++) {
		workers[i].img = &img;
		worker_start(&workers[i]);
	}

	// ---- render phase: status until every worker has finished or died
	int finished = 0;
	unsigned elapsed = 0;
	while (SYS_MainLoop() && !finished) {
		finished = 1;
		for (int i = 0; i < NUM_WORKERS; i++)
			if (workers[i].state != W_DONE && workers[i].state != W_DEAD) finished = 0;

		elapsed = (unsigned)ticks_to_secs(gettime() - t0);
		uint32_t done, total;
		sched_progress(&done, &total);
		print_status(my_ip, elapsed, done >= total, finished);

		VIDEO_WaitVSync();
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) return 0;	// threads die with the process
	}
	if (!finished) return 0;				// SYS_MainLoop said exit (reset / power)

	uint32_t done, total;
	sched_progress(&done, &total);
	int complete = done >= total;

	// ---- result phase: show the image, A toggles status
	tonemap(img.sum, rgb);
	draw_image(rgb);
	int show_img = 1;

	while (SYS_MainLoop()) {
		VIDEO_WaitVSync();
		WPAD_ScanPads();
		u32 down = WPAD_ButtonsDown(0);

		if (down & WPAD_BUTTON_HOME) break;
		if (down & WPAD_BUTTON_A) {
			show_img = !show_img;
			if (show_img) {
				draw_image(rgb);
			} else {
				VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
				print_status(my_ip, elapsed, complete, 1);
				printf("A: show image   HOME: quit\n");
			}
		}
	}
	return 0;
}
