// Wii head node: splits a raytrace into tiles, farms them out to worker Wiis
// (node.c) over TCP, and shows the image as it comes together.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/if_config.h>

#include "config.h"
#include "coordinator.h"

#define IMG_Y0		20			// image top edge on screen, in pixels
#define IMG_SCALE	2			// every image pixel is drawn as 2x2

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static uint32_t *fb;			// framebuffer as YCbCr pixel pairs
static int stride;				// u32 per screen row
static int img_x0;				// image left edge in u32 columns

static coordinator_t coord;
static uint8_t shown[N_TILES];	// what each tile currently looks like on screen

static const char *worker_state_name(worker_state_t s) {
	switch (s) {
		case W_CONNECTING: return "connecting";
		case W_IDLE:       return "idle      ";
		case W_BUSY:       return "rendering ";
		case W_DEAD:       return "DEAD      ";
		default:           return "finished  ";
	}
}

// One u32 is two horizontally adjacent pixels (Y0 Cb Y1 Cr); we draw 2x wide,
// so both get the same colour.
static inline uint32_t yuy2(int r, int g, int b) {
	int y  = (( 66 * r + 129 * g +  25 * b + 128) >> 8) + 16;
	int cb = ((-38 * r -  74 * g + 112 * b + 128) >> 8) + 128;
	int cr = ((112 * r -  94 * g -  18 * b + 128) >> 8) + 128;
	return ((uint32_t)y << 24) | ((uint32_t)cb << 16) | ((uint32_t)y << 8) | (uint32_t)cr;
}

static inline void plot(int x, int y, uint32_t v) {
	uint32_t *row = fb + (IMG_Y0 + y * IMG_SCALE) * stride + img_x0 + x;
	row[0] = v;
	row[stride] = v;
}

// Tiles in progress get a colour per worker so you can see the work being shared.
static const uint8_t palette[][3] = {
	{ 30, 60, 140 }, { 30, 110, 60 }, { 140, 100, 30 }, { 110, 40, 110 },
	{ 30, 110, 110 }, { 140, 50, 50 }, { 90, 90, 90 }, { 60, 60, 130 }
};

static void draw_tile(int idx, tile_state_t st, int owner) {
	int tx, ty, tw, th;
	tile_rect(idx, &tx, &ty, &tw, &th);

	uint32_t flat = yuy2(0, 0, 0);
	if (st == TILE_ACTIVE) {
		const uint8_t *p = palette[owner % 8];
		flat = yuy2(p[0], p[1], p[2]);
	} else if (st == TILE_FAILED) {
		flat = yuy2(200, 0, 0);
	}

	for (int y = 0; y < th; y++) {
		for (int x = 0; x < tw; x++) {
			uint32_t v = flat;
			if (st == TILE_DONE) {
				const uint8_t *p = &coord.pixels[((ty + y) * IMG_W + tx + x) * 3];
				v = yuy2(p[0], p[1], p[2]);
			}
			plot(tx + x, ty + y, v);
		}
	}
}

static void refresh_tiles(void) {
	for (int i = 0; i < N_TILES; i++) {
		int owner;
		tile_state_t st = coordinator_tile(&coord, i, &owner);
		uint8_t key = (uint8_t)(st * 32 + (st == TILE_ACTIVE ? owner : 0));
		if (key != shown[i]) {
			draw_tile(i, st, owner);
			shown[i] = key;
		}
	}
}

static void print_status(const char *local_ip) {
	uint32_t ms = coordinator_elapsed_ms(&coord);

	printf("\x1b[0;0H");
	printf("Wii head node  %-15s  HOME quit%s\n", local_ip,
	       coordinator_done(&coord) ? ", A re-render" : "              ");
	printf("tiles %u/%u  failed %u  time %u.%01us  %s\n",
	       (unsigned)coord.tiles_done, (unsigned)N_TILES, (unsigned)coord.tiles_failed,
	       (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100),
	       coordinator_done(&coord) ? "DONE   " : "running");

	for (int i = 0; i < coord.n_workers; i++) {
		worker_t *w = &coord.workers[i];
		printf("%-15s %s tiles %-3u err %-2u (%d)   \n", w->ip, worker_state_name(w->state),
		       (unsigned)w->tiles_done, (unsigned)w->errors, w->last_error);
	}
}

static void init_video(void) {
	VIDEO_Init();
	WPAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);

	// Text lives below the image; the image is drawn straight into the XFB.
	int text_y = IMG_Y0 + IMG_H * IMG_SCALE + 10;
	console_init(xfb, 20, text_y, rmode->fbWidth - 40, rmode->xfbHeight - text_y - 20,
	             rmode->fbWidth * VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

	fb = (uint32_t *)xfb;
	stride = rmode->fbWidth / 2;
	img_x0 = (stride - IMG_W) / 2;
}

int main(int argc, char **argv) {
	init_video();

	printf("Wii head node\n\nconfiguring network...\n");

	char localip[16] = {0}, netmask[16] = {0}, gateway[16] = {0};
	if (if_config(localip, netmask, gateway, true, 20) < 0) {
		printf("network configuration failed\n\nHOME to quit\n");
		while (SYS_MainLoop()) {
			VIDEO_WaitVSync();
			WPAD_ScanPads();
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
		}
		return 1;
	}

	render_params_t params = {
		.cam_pos    = { 0.0f, 1.0f, -5.0f },
		.cam_target = { 0.0f, 0.0f,  0.0f },
		.fov_deg    = 60.0f,
		.scene_id   = 0,
		.samples    = 1
	};

	coordinator_init(&coord, WORKER_IPS, (int)N_WORKERS, WORKER_PORT, &params);
	memset(shown, 0xFF, sizeof shown);
	coordinator_start(&coord);

	while (SYS_MainLoop()) {
		refresh_tiles();
		print_status(localip);

		VIDEO_WaitVSync();
		WPAD_ScanPads();
		u32 down = WPAD_ButtonsDown(0);

		if (down & WPAD_BUTTON_HOME) break;

		if ((down & WPAD_BUTTON_A) && coordinator_done(&coord)) {
			coordinator_join(&coord);
			memset(shown, 0xFF, sizeof shown);
			coordinator_start(&coord);
		}
	}

	coordinator_stop(&coord);
	return 0;		// worker threads may be blocked in recv(); process exit ends them
}
