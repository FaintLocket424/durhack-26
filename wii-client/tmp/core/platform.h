//---------------------------------------------------------------------------------
// platform.h - the ONLY interface between the portable core and a target.
//
// Core (never touches a console/OS API):  protocol.h  render.[ch]  handlers.[ch]
//                                          node.[ch]
// Per target you provide:                  main() + the functions below.
//
// Threading model
//   The core is happy with ONE thread. node_run() blocks, and whenever it has
//   nothing to do (waiting for a client / for bytes) it calls plat_idle(), so a
//   single-threaded target (DS, Arduino, Wii U) can pump its UI/watchdog there
//   and from plat_preview(). A multi-threaded target (Wii) may instead run
//   node_run() on its own thread and ignore plat_idle().
//
// Blocking vs polled network
//   plat_accept/recv/send may block (then they never return PLAT_AGAIN), or
//   may return PLAT_AGAIN after a short timeout (~10-100 ms) so the core can
//   call plat_idle() and re-check plat_keep_running().
//---------------------------------------------------------------------------------
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "render.h"				// tile_job_t

#define PLAT_AGAIN	(-30000)	// "nothing yet, call plat_idle() and retry"

typedef int plat_conn_t;		// opaque connection handle, >= 0

// ---- lifecycle ----------------------------------------------------------------
// Bring the network up (DHCP / WiFi join). Write the local IP as text to ip_out.
// Returns 0 or a negative error code (stored in node->last_error).
int  plat_net_init(char *ip_out, size_t len);

// False once the user/OS wants the program to exit (HOME, power, window close).
bool plat_keep_running(void);

// Called whenever the node is waiting. Pump UI, feed the watchdog, etc. May be empty.
void plat_idle(void);

void plat_sleep_ms(unsigned ms);

// ---- network (one listener, one client at a time) -------------------------------
int  plat_listen(uint16_t port);			// 0, or negative error
void plat_unlisten(void);
int  plat_accept(void);						// conn >= 0 | PLAT_AGAIN | negative error
int  plat_recv(plat_conn_t c, void *buf, size_t len);		// >0 bytes | PLAT_AGAIN | -1 closed/error
int  plat_send(plat_conn_t c, const void *buf, size_t len);	// >0 bytes | PLAT_AGAIN | -1 error
void plat_close(plat_conn_t c);

// ---- preview (optional; may be a stub that returns 0) ----------------------------
// Called after every 1-spp pass of a tile, from whichever thread renders.
// accum = w*h*3 running SUMS; divide by spp_done, sqrt for gamma, then draw.
// Return non-zero to abort the render.
int  plat_preview(const tile_job_t *job, const float *accum, uint32_t spp_done);

#ifdef __cplusplus
}
#endif
#endif
