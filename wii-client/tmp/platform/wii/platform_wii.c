// Wii (devkitPPC + libogc). Network runs on its own LWP thread, UI on the main one.
// Build flat: copy core/*, platform/posix/net_bsd.c and platform/wii/* into source/.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/if_config.h>

#include "node.h"
#include "platform.h"
#include "view.h"

#define NODE_PORT 5555

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static node_t node = { .port = NODE_PORT };
static lwp_t node_handle = (lwp_t)NULL;

// ---- platform.h ----------------------------------------------------------------
int plat_net_init(char *ip, size_t len) {
	char localip[16] = {0}, gateway[16] = {0}, netmask[16] = {0};
	int ret = if_config(localip, netmask, gateway, true, 20);	// DHCP, 20 retries
	if (ret < 0) return ret;
	strncpy(ip, localip, len - 1);
	return 0;
}
bool plat_keep_running(void)    { return SYS_MainLoop(); }	// false on HOME / reset / power
void plat_idle(void)            { }						// blocking sockets, own thread
void plat_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
int  plat_preview(const tile_job_t *j, const float *a, uint32_t n) { return view_update(j, a, n); }

// ---- UI ------------------------------------------------------------------------
static void *initialise(void) {
	VIDEO_Init();
	WPAD_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	void *fb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(fb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(fb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
	return fb;
}

static const char *state_name(node_state_t s) {
	switch (s) {
		case NODE_STARTING:  return "configuring network";
		case NODE_LISTENING: return "listening          ";
		case NODE_CONNECTED: return "client connected   ";
		default:             return "FAILED             ";
	}
}

int main(int argc, char **argv) {
	xfb = initialise();
	view_init();

	LWP_CreateThread(&node_handle, node_run, &node, NULL, 64 * 1024, 50);

	const int W = rmode->fbWidth, H = rmode->xfbHeight;
	int showing_tile = 0;

	while (SYS_MainLoop()) {
		int want_tile = view_has_tile() && node.state == NODE_CONNECTED;
		if (want_tile) {
			if (!showing_tile) { showing_tile = 1; view_invalidate(); }
			view_present((uint32_t *)xfb, W, H);
		} else {
			if (showing_tile) { showing_tile = 0; VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK); }
			printf("\x1b[2;0H");
			printf("Wii compute node\n\n");
			printf("state:        %s\n", state_name(node.state));
			printf("address:      %s:%d\n", node.local_ip, node.port);
			printf("connections:  %u\n", (unsigned)node.connections);
			printf("requests:     %u\n", (unsigned)node.requests);
			printf("errors:       %u (last: %d)\n", (unsigned)node.errors, node.last_error);
			printf("\nHOME to quit\n");
		}
		VIDEO_WaitVSync();
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
	}

	view_request_quit();
	node.stop = 1;
	return 0;
}
