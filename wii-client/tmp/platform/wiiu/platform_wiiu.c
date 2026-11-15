// Wii U (devkitPPC + wut). Big-endian PPC like the Wii, so the wire format is
// identical. Single thread; the process-UI loop is polled via plat_keep_running().
// Build with -DPLAT_NET_POLLED. Preview is just a log line here: add GX2/SDL
// drawing inside plat_preview() when you want pixels on screen.
// Verify the wut headers/calls against your wut version.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <coreinit/thread.h>
#include <nsysnet/socket.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>

#include "node.h"
#include "platform.h"

static node_t node = { .port = 5555 };

int plat_net_init(char *ip, size_t len) {
	socket_lib_init();
	strncpy(ip, "see router", len - 1);					// nn::ac can fetch the real IP
	return 0;
}

bool plat_keep_running(void)    { return WHBProcIsRunning(); }
void plat_idle(void)            { }
void plat_sleep_ms(unsigned ms) { OSSleepTicks(OSMillisecondsToTicks(ms)); }

int plat_preview(const tile_job_t *job, const float *accum, uint32_t spp_done) {
	(void)accum;
	WHBLogPrintf("tile (%u,%u) pass %u/%u", (unsigned)job->x0, (unsigned)job->y0,
	             (unsigned)spp_done, (unsigned)job->spp);
	WHBLogConsoleDraw();
	return 0;
}

int main(void) {
	WHBProcInit();
	WHBLogConsoleInit();
	WHBLogPrintf("Wii U compute node, port %u", node.port);
	WHBLogConsoleDraw();

	node_run(&node);

	WHBLogConsoleFree();
	WHBProcShutdown();
	return 0;
}
