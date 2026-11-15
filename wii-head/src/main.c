// Wii head node demo: configure network, then ping all workers once a second.

#include <stdio.h>
#include <string.h>

#include <ogcsys.h>
#include <gccore.h>
#include <ogc/if_config.h>
#include <wiiuse/wpad.h>

#include "head.h"

#define WORKER_PORT		5555
#define PING_TIMEOUT_MS	2000

// they are currently dynamic.... need to be static
static const char *worker_ips[] = {
	"192.168.1.50",
	"192.168.1.51",
	"192.168.1.52",
};
#define NUM_WORKERS (int)(sizeof worker_ips / sizeof worker_ips[0])

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

void *initialise();

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	char localip[16] = {0};
	char gateway[16] = {0};
	char netmask[16] = {0};

	xfb = initialise();

	printf("\nWii head node\nConfiguring network ...\n");
	if (if_config(localip, netmask, gateway, true, 20) < 0) {
		printf("network configuration failed!\n");
		while (SYS_MainLoop()) { VIDEO_WaitVSync(); }
		return 1;
	}
	printf("ip: %s\n", localip);

	worker_t workers[HEAD_MAX_WORKERS];
	for (int i = 0; i < NUM_WORKERS; i++)
		head_worker_init(&workers[i], worker_ips[i], WORKER_PORT);

	const uint8_t payload[] = "hello wii";
	unsigned round = 0;

	while (SYS_MainLoop()) {
		// Blocks for at most PING_TIMEOUT_MS; the UI just pauses meanwhile.
		int ok = head_ping(workers, NUM_WORKERS, payload, sizeof payload, PING_TIMEOUT_MS);

		printf("\x1b[5;0H");
		printf("round %u: %d/%d workers answered     \n\n", ++round, ok, NUM_WORKERS);
		for (int i = 0; i < NUM_WORKERS; i++) {
			printf("%-15s %-10s", workers[i].ip, head_state_name(workers[i].state));
			if (workers[i].state == WORKER_OK) printf(" %3u ms      \n", (unsigned)workers[i].rtt_ms);
			else                               printf(" err %-4d    \n", workers[i].error);
		}
		printf("\nHOME to quit\n");

		// ~1 s between rounds, polling the pad so HOME stays responsive
		for (int f = 0; f < 60; f++) {
			VIDEO_WaitVSync();
			WPAD_ScanPads();
			if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) goto done;
		}
	}
done:
	for (int i = 0; i < NUM_WORKERS; i++) head_close(&workers[i]);
	return 0;
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
