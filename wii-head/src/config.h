//---------------------------------------------------------------------------------
// config.h - edit this before building the head node.
//---------------------------------------------------------------------------------
#ifndef CONFIG_H
#define CONFIG_H

// Image and tile size in pixels. The image is shown pixel-doubled, so
// IMG_W must be <= 320. IMG_H * 2 + 70 must fit the screen; leave room below
// the image for one status line per worker.
#define IMG_W				256
#define IMG_H				160
#define TILE_W				32
#define TILE_H				32

#define WORKER_PORT			5555
#define MAX_WORKERS			4

#define MAX_TILE_ATTEMPTS	3		// tile is given up on after this many failures
#define CONNECT_RETRIES		30		// ~0.5 s apart; worker is then marked dead
#define RECV_TIMEOUT_S		60		// longest a worker may take to answer one tile

// Worker Wiis (the ones running node.c); see their screen for the address.
static const char *const WORKER_IPS[] = {
	"192.168.1.101",
	"192.168.1.102",
};
#define N_WORKERS	(sizeof WORKER_IPS / sizeof WORKER_IPS[0])

#if IMG_W > 320
#error "IMG_W must be <= 320 (image is drawn at 2x on a 640 wide screen)"
#endif

#endif
