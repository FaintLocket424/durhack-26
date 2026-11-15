// Nintendo DS (devkitARM + libnds + dswifi). Single thread: node_run() runs on
// the main thread and everything else happens in plat_idle()/plat_preview().
// Build with -DPLAT_NET_POLLED -DNDS (net_bsd.c then uses select() timeouts).
//
// Caveats: ARM946E-S @ 67 MHz with NO FPU, so every float op is software emulated.
// Expect this to be 1-2 orders of magnitude slower than the Wii. Keep tiles tiny
// (plat_config.h: RENDER_MAX_PIXELS 256) and use the DS for a few spp per request.
// 4 MB RAM total.  Verify the dswifi calls below against your libnds/dswifi version.
#include <nds.h>
#include <dswifi9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <arpa/inet.h>

#include "node.h"
#include "platform.h"

static node_t node = { .port = 5555 };
static volatile bool quit = false;
static u16 small[RENDER_MAX_PIXELS];				// tile as RGB15, before upscaling

int plat_net_init(char *ip, size_t len) {
	if (!Wifi_InitDefault(WFC_CONNECT)) return -1;		// uses the console's WFC settings
	struct in_addr a = Wifi_GetIP();
	strncpy(ip, inet_ntoa(a), len - 1);
	return 0;
}

bool plat_keep_running(void) { return !quit; }

void plat_idle(void) {
	scanKeys();
	if (keysDown() & KEY_START) quit = true;
	swiWaitForVBlank();									// ~16 ms, saves battery too
}

void plat_sleep_ms(unsigned ms) { for (unsigned t = 0; t < ms; t += 17) swiWaitForVBlank(); }

// Top-left 256x192 direct-colour framebuffer, nearest-neighbour upscale of the tile.
int plat_preview(const tile_job_t *job, const float *accum, uint32_t spp_done) {
	float inv = 1.0f / (float)spp_done;
	int n = (int)(job->w * job->h);
	for (int i = 0; i < n; i++) {
		int r = (int)(sqrtf(accum[i*3+0] * inv) * 31.0f); if (r > 31) r = 31;
		int g = (int)(sqrtf(accum[i*3+1] * inv) * 31.0f); if (g > 31) g = 31;
		int b = (int)(sqrtf(accum[i*3+2] * inv) * 31.0f); if (b > 31) b = 31;
		small[i] = RGB15(r, g, b) | BIT(15);
	}
	for (int y = 0; y < 192; y++) {
		const u16 *src = small + (y * job->h / 192) * job->w;
		u16 *dst = VRAM_A + y * 256;
		for (int x = 0; x < 256; x++) dst[x] = src[x * job->w / 256];
	}
	scanKeys();
	if (keysDown() & KEY_START) quit = true;
	return quit;
}

int main(void) {
	videoSetMode(MODE_FB0);								// main screen: preview
	vramSetBankA(VRAM_A_LCD);
	consoleDemoInit();									// sub screen: text
	printf("DS compute node\nSTART to quit\n\nconnecting WiFi...\n");

	node_run(&node);

	printf("stopped (err %d)\n", node.last_error);
	return 0;
}
