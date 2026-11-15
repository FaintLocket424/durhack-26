// 3DS compute node: UI (top = tile preview, bottom = status) + network thread (node_run)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "node.h"
#include "view.h"

#define NODE_PORT	5555
#define TOP_W		400
#define TOP_H		240

static node_t node = { .port = NODE_PORT };
static Thread node_thread = NULL;

// libctru thread entries return void, node_run returns void *
static void node_entry(void *arg) { node_run(arg); }

//---------------------------------------------------------------------------------
static const char *state_name(node_state_t state) {
//---------------------------------------------------------------------------------
	switch (state) {
		case NODE_STARTING:  return "configuring network";
		case NODE_LISTENING: return "listening          ";
		case NODE_CONNECTED: return "client connected   ";
		default:             return "FAILED             ";
	}
}

//---------------------------------------------------------------------------------
static void clear_top(void) {
//---------------------------------------------------------------------------------
	u16 w, h;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
	memset(fb, 0, (size_t)w * h * 3);
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	osSetSpeedupEnable(true);				// New 3DS: 804 MHz + L2 cache; no-op on Old 3DS

	gfxInit(GSP_BGR8_OES, GSP_RGB565_OES, false);
	gfxSetDoubleBuffering(GFX_TOP, false);	// we only redraw when the tile changes
	consoleInit(GFX_BOTTOM, NULL);
	clear_top();

	view_init();							// hook the preview into the renderer

	// Lower priority than the main thread, so the UI wakes up on time even while
	// a render is hogging the CPU. (Higher priority would starve it.)
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	node_thread = threadCreate(node_entry, &node, 128 * 1024, prio + 1, -2, false);

	int showing_tile = 0;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;

		// Tile on the top screen while a coordinator is connected, black otherwise.
		int want_tile = view_has_tile() && node.state == NODE_CONNECTED;

		if (want_tile) {
			if (!showing_tile) {
				showing_tile = 1;
				view_invalidate();
			}
			u16 w, h;
			u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
			view_present(fb, TOP_W, TOP_H);	// only redraws when the tile changed
		} else if (showing_tile) {
			showing_tile = 0;
			clear_top();
		}

		printf("\x1b[1;1H");
		printf("3DS compute node\n\n");
		printf("state:        %s\n", state_name(node.state));
		printf("address:      %s:%d      \n", node.local_ip, node.port);
		printf("connections:  %u      \n", (unsigned)node.connections);
		printf("requests:     %u      \n", (unsigned)node.requests);
		printf("errors:       %u (last: 0x%08x)  \n", (unsigned)node.errors, (unsigned)node.last_error);
		printf("\nSTART to quit\n");

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	view_request_quit();					// abort any render in progress
	node.stop = 1;
	if (node_thread) {						// wakes within ~250 ms; closes sockets, socExit()
		threadJoin(node_thread, U64_MAX);
		threadFree(node_thread);
	}

	gfxExit();
	return 0;
}
