//---------------------------------------------------------------------------------
// node.h - TCP front end: accept a coordinator, read frames, dispatch, reply.
//
// Same behaviour and wire protocol as wii/src/node.c, but libxenon has no
// threads and its lwIP has no BSD sockets (NO_SYS=1, LWIP_SOCKET=0), so this
// uses lwIP's raw callback API. Everything happens inside network_poll(),
// which main() calls in its loop.
//---------------------------------------------------------------------------------
#ifndef NODE_H
#define NODE_H

#include <stdint.h>

typedef enum {
  NODE_STARTING, // configuring the network (DHCP)
  NODE_LISTENING,
  NODE_CONNECTED,
  NODE_FAILED
} node_state_t;

typedef struct {
  uint16_t port; // set before node_start()

  node_state_t state;
  int last_error; // lwIP err_t or proto_status_t
  uint32_t connections;
  uint32_t requests;
  uint32_t errors;
  char local_ip[16];
} node_t;

// Brings up the network (DHCP) and starts listening on node->port.
// Returns 0 on success, negative on failure (node->state == NODE_FAILED).
int node_start(node_t *node);

// Call every iteration of the main loop: drives the NIC, lwIP timers and
// all the TCP callbacks below.
void node_poll(node_t *node);

// Close the client connection and the listener.
void node_stop(node_t *node);

#endif
