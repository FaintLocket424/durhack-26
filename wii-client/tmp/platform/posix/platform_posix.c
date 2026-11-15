// PC build: dev loop for everything above the platform line.
//   ./node_host [port]
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "node.h"
#include "platform.h"

static volatile sig_atomic_t quit = 0;
static void on_sigint(int s) { (void)s; quit = 1; }

int  plat_net_init(char *ip, size_t len) { strncpy(ip, "0.0.0.0", len - 1); return 0; }
bool plat_keep_running(void)             { return !quit; }
void plat_idle(void)                     { }
void plat_sleep_ms(unsigned ms)          { usleep(ms * 1000u); }
int  plat_preview(const tile_job_t *j, const float *a, uint32_t n) { (void)j; (void)a; (void)n; return 0; }

int main(int argc, char **argv) {
	static node_t node;
	node.port = argc > 1 ? (uint16_t)atoi(argv[1]) : 5555;
	signal(SIGINT, on_sigint);
	printf("node listening on :%u (Ctrl-C to stop)\n", node.port);
	node_run(&node);
	printf("requests %u, errors %u\n", (unsigned)node.requests, (unsigned)node.errors);
	return 0;
}
