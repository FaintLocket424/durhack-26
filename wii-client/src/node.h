//---------------------------------------------------------------------------------
// node.h - TCP front end: accept a coordinator, read frames, dispatch, reply.
//
// The same code builds for the Wii (-DGEKKO) and for a PC (see host/).
//---------------------------------------------------------------------------------
#ifndef NODE_H
#define NODE_H

#include <stdint.h>

typedef enum {
	NODE_STARTING,			// configuring the network (DHCP)
	NODE_LISTENING,
	NODE_CONNECTED,
	NODE_FAILED
} node_state_t;

// Owned by the network thread. The UI thread only reads it, so plain volatile
// fields are enough for a status display.
typedef struct {
	uint16_t			port;				// set before starting the thread
	volatile int		stop;				// set to 1 to ask the thread to finish

	volatile node_state_t	state;
	volatile int		last_error;			// errno or proto_status_t
	volatile uint32_t	connections;
	volatile uint32_t	requests;
	volatile uint32_t	errors;
	char				local_ip[16];
} node_t;

// Thread entry point (same shape as httpd() in the wii-examples sockettest).
// arg is a node_t *. Blocks until node->stop is set or the system asks to exit.
void *node_run(void *arg);

#endif
