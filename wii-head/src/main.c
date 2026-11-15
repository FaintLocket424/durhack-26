// Wii render coordinator: splits the image into tiles, farms them out to the
// nodes in config.h, prints status until everything is back, then shows the image.
//   A    toggle between image and status
//   HOME quit

#include <stdio.h>
#include <stdarg.h>
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
#include "log.h"
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

#define SCREEN_COLS	74					// 640px - 2*20px margin, 8px font
#define SCREEN_ROWS	25
#define STATUS_EVERY	12					// redraw the status every 12 frames (~5 Hz)

//---------------------------------------------------------------------------------
static const char *state_name(worker_state_t s) {
//---------------------------------------------------------------------------------
	switch (s) {
		case W_CONNECTING: return "connect";
		case W_RENDERING:  return "render";
		case W_IDLE:       return "idle";
		case W_BACKOFF:    return "retry";
		case W_DONE:       return "done";
		default:           return "DEAD";
	}
}

// One full-width console line; padding overwrites whatever was there last frame.
//---------------------------------------------------------------------------------
static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...) {
//---------------------------------------------------------------------------------
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	printf("%-*.*s\n", SCREEN_COLS, SCREEN_COLS, buf);
}

//---------------------------------------------------------------------------------
// Per node, two lines:
//   ip  state  tiles done, requests queued on the node, last tile's node time
//   (render + latency) and transfer time, reply throughput, render speed in
//   thousands of samples/s, share of the finished work
//   tile in progress, pixel format, connects (c) / failures (f), seconds since
//   the node last sent a byte, last error (full text is in the event log)
//---------------------------------------------------------------------------------
static void print_status(const char *my_ip, unsigned elapsed_ms, int complete, int finished) {
//---------------------------------------------------------------------------------
	sched_stats_t st;
	sched_stats(&st);
	uint32_t now = worker_now_ms();

	printf("\x1b[2;0H");
	line("Wii render coordinator (%s)", my_ip);
	line("image %dx%d, %d passes x %d spp, depth %d, %d nodes",
	     IMG_W, IMG_H, PASSES, SPP_PER_REQ, MAX_DEPTH, NUM_WORKERS);
	line("%s", "");
	line("node            state   tiles q node ms xfer ms  KB/s  ksps share");

	for (int i = 0; i < NUM_WORKERS; i++) {
		const worker_t *w = &workers[i];
		unsigned share = st.done ? (unsigned)((uint64_t)w->px_done * 100 / st.done) : 0;
		unsigned ksps  = w->node_ms_total ?
		                 (unsigned)((uint64_t)w->px_done * SPP_PER_REQ / w->node_ms_total) : 0;

		line("%-15s %-7s %5u %u %7u %7u %5u %5u %4u%%", w->ip, state_name(w->state),
		     (unsigned)w->tiles, (unsigned)w->in_flight, (unsigned)w->node_ms,
		     (unsigned)w->xfer_ms, (unsigned)w->kbps, ksps, share);

		char tile[24] = "-";
		if (w->state == W_RENDERING)
			snprintf(tile, sizeof tile, "@%u,%u %ux%u", (unsigned)w->cur_x, (unsigned)w->cur_y,
			         (unsigned)w->cur_w, (unsigned)w->cur_h);
		unsigned quiet = w->state == W_RENDERING && w->last_rx_ms ?
		                 (unsigned)((now - w->last_rx_ms) / 1000) : 0;
		if (w->state == W_CONNECTING && w->conn_start_ms)		// "connect 40s" = wrong IP / node not reachable
			snprintf(tile, sizeof tile, "connect %us", (unsigned)((now - w->conn_start_ms) / 1000));

		line("  %-14s %s c%u f%u quiet %2us %s", tile,
		     w->tiles ? (w->f16 ? "f16" : "f32") : "---",
		     (unsigned)w->connects, (unsigned)w->failures, quiet, w->err);
	}

	line("%s", "");
	unsigned permille = st.total ? (unsigned)((uint64_t)st.done * 1000 / st.total) : 0;
	char lost[24] = "";
	if (st.lost) snprintf(lost, sizeof lost, "  LOST %u", (unsigned)st.lost);
	line("progress %3u.%u%%  pass %u/%u  in flight %u  requeued %u  waiting %u%s",
	     permille / 10, permille % 10,
	     (unsigned)(st.pass < st.passes ? st.pass + 1 : st.passes), (unsigned)st.passes,
	     (unsigned)st.in_flight, (unsigned)st.requeued, (unsigned)st.queued, lost);

	char eta[16] = "--";
	if (st.done > 0 && st.done < st.total)
		snprintf(eta, sizeof eta, "%us", (unsigned)((uint64_t)elapsed_ms * (st.total - st.done) / st.done / 1000));
	unsigned cluster = elapsed_ms ? (unsigned)((uint64_t)st.done * SPP_PER_REQ / elapsed_ms) : 0;
	line("elapsed %u.%us  ETA %s  cluster %u ksps", elapsed_ms / 1000, (elapsed_ms % 1000) / 100, eta, cluster);

	// event log fills the rest of the screen
	int show = SCREEN_ROWS - (10 + 2 * NUM_WORKERS) - 1;
	if (show < 3) show = 3;
	if (show > 12) show = 12;
	char ev[12][LOG_COLS + 1];
	int n = log_copy(ev, show);
	line("%s", "");
	line("events:");
	for (int i = 0; i < show; i++) line("%s", i < n ? ev[i] : "");

	if (finished)
		line("%s", complete ? "complete." : "INCOMPLETE: all nodes dropped out.");

	unsigned sweeps = st.done / (IMG_W * IMG_H);
	printf("\nsweeps: %u   spp: ~%u   elapsed: %us   \n", sweeps, sweeps * SPP_PER_REQ, elapsed_ms);
	if (finished)
		printf("all nodes dropped out.              \n\n");
	else
		line("HOME to quit");
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

static void tonemap(image_t *img, uint8_t *rgb) {
	LWP_MutexLock(img->lock);
	for (int p = 0; p < IMG_W * IMG_H; p++) {
		float inv = img->cnt[p] ? 1.0f / (float)(img->cnt[p] * SPP_PER_REQ) : 0.0f;
		for (int c = 0; c < 3; c++)
			rgb[p * 3 + c] = (uint8_t)clamp255((int)(sqrtf(img->sum[p * 3 + c] * inv) * 255.0f + 0.5f));
	}
	LWP_MutexUnlock(img->lock);
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
	//printf("Wii render coordinator\n\nconfiguring network... ");

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
	img.cnt = calloc(IMG_W * IMG_H, sizeof(uint32_t));
	uint8_t *rgb = malloc(IMG_W * IMG_H * 3);
	if (!img.sum || !img.cnt || !rgb) {
		printf("out of memory\n");
		wait_home();
		return 1;
	}
	LWP_MutexInit(&img.lock, false);

	sched_init(IMG_W, IMG_H, PASSES);
	log_init();

	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
	u64 t0 = gettime();
	for (int i = 0; i < NUM_WORKERS; i++) {
		workers[i].img = &img;
		worker_start(&workers[i]);
	}

	// ---- render phase: image is the main view, A toggles the status overlay.
    // Tonemapping and repacking the XFB is CPU heavy, so keep the UI thread
    // below the workers and only redraw about once a second.
    LWP_SetThreadPriority(LWP_GetSelf(), 40);

    int finished = 0, show_info = 0, redraw = 1;
    unsigned elapsed = 0;                   // ms
    unsigned frame = 0;
    uint32_t last_done = (uint32_t)-1;

    while (SYS_MainLoop()) {
        finished = 1;
        for (int i = 0; i < NUM_WORKERS; i++)
            if (workers[i].state != W_DONE && workers[i].state != W_DEAD) finished = 0;

        uint32_t done, total;
        sched_progress(&done, &total);
        if (!finished) elapsed = (unsigned)ticks_to_millisecs(gettime() - t0);

        // refresh about once a second while rendering, and once more at the end
        if (++frame % 60 == 0 && done != last_done) redraw = 1;
        if (finished && last_done != done) redraw = 1;

        if (redraw) {
            tonemap(&img, rgb);
            draw_image(rgb);
            if (show_info)
                print_status(my_ip, elapsed, done >= total, finished);
            last_done = done;
            redraw = 0;
        }

        VIDEO_WaitVSync();
        WPAD_ScanPads();
        u32 down = WPAD_ButtonsDown(0);
        if (down & WPAD_BUTTON_HOME) break;
        if (down & WPAD_BUTTON_A) { show_info = !show_info; redraw = 1; }
    }
    return 0;
}