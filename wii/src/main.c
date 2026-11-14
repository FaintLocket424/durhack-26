// Wii compute node: UI thread (status screen) + network thread (node_run)

#include <stdio.h>
#include <stdlib.h>

#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include "node.h"

#define NODE_PORT	5555

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static node_t node = { .port = NODE_PORT };
static lwp_t node_handle = (lwp_t)NULL;

void *initialise();

//---------------------------------------------------------------------------------
static const char *state_name(node_state_t state) {
//---------------------------------------------------------------------------------
	switch (state) {
		case NODE_STARTING:  return "configuring network";
		case NODE_LISTENING: return "listening         ";
		case NODE_CONNECTED: return "client connected  ";
		default:             return "FAILED            ";
	}
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	xfb = initialise();

	LWP_CreateThread(	&node_handle,	/* thread handle */
						node_run,		/* code */
						&node,			/* arg pointer for thread */
						NULL,			/* stack base */
						64*1024,		/* stack size */
						50				/* thread priority */ );

	while(SYS_MainLoop()) {

		printf("\x1b[2;0H");
		printf("Wii compute node\n\n");
		printf("state:        %s\n", state_name(node.state));
		printf("address:      %s:%d\n", node.local_ip, node.port);
		printf("connections:  %u\n", (unsigned)node.connections);
		printf("requests:     %u\n", (unsigned)node.requests);
		printf("errors:       %u (last: %d)\n", (unsigned)node.errors, node.last_error);
		printf("\nHOME to quit\n");

		VIDEO_WaitVSync();
		WPAD_ScanPads();

		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;
	}

	node.stop = 1;
	return 0;		// the thread may be blocked in accept(); process exit ends it
}

//---------------------------------------------------------------------------------
void *initialise() {
//---------------------------------------------------------------------------------

	void *framebuffer;

	VIDEO_Init();
	WPAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(framebuffer,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

	return framebuffer;

}
