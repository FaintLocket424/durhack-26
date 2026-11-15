//---------------------------------------------------------------------------------
// head.h - coordinator side: talk to several worker nodes at once.
//
// Everything runs on ONE thread with non-blocking sockets + select(), so a slow
// or dead worker can never stall the others, and no call can block past its
// timeout. No malloc() is used (fixed buffers inside worker_t).
//---------------------------------------------------------------------------------
#ifndef HEAD_H
#define HEAD_H

#include <stdint.h>
#include "protocol.h"

#define HEAD_MAX_WORKERS		8		// libogc has a small socket pool, keep this modest
#define HEAD_PING_MAX_PAYLOAD	256

typedef enum {
	WORKER_IDLE,			// not connected / ready to start
	WORKER_CONNECTING,		// connect() in progress
	WORKER_SENDING,			// request partly written
	WORKER_RECV_HEADER,		// waiting for the 16 byte header
	WORKER_RECV_PAYLOAD,	// waiting for the rest
	WORKER_OK,				// valid PONG received; socket stays open for reuse
	WORKER_FAILED,			// see .error
	WORKER_TIMEOUT
} worker_state_t;

typedef struct {
	// --- set with head_worker_init() ---
	char		ip[16];
	uint16_t	port;

	// --- results, valid after head_ping() ---
	worker_state_t	state;
	int				error;			// errno (>0) or proto_status_t (<0), 0 if none
	uint32_t		rtt_ms;			// round trip, only meaningful when state == WORKER_OK

	// --- internal ---
	int			sock;				// -1 when closed
	uint32_t	next_id;
	uint64_t	t_start;
	uint8_t		tx[PROTO_HEADER_SIZE + HEAD_PING_MAX_PAYLOAD];
	uint32_t	tx_len, tx_off;
	uint8_t		rx_hdr[PROTO_HEADER_SIZE];
	uint32_t	rx_off;				// bytes of the current part (header or payload) read
	uint8_t		rx_payload[HEAD_PING_MAX_PAYLOAD];
	frame_header_t rx_h;
} worker_t;

void head_worker_init(worker_t *w, const char *ip, uint16_t port);

// Ping every worker in parallel and wait for all PONGs (or the timeout).
// Connections that succeed are kept open and reused by the next call.
// Returns the number of workers in WORKER_OK, or a negative errno on bad arguments.
int head_ping(worker_t *workers, int n, const uint8_t *payload, uint32_t len, uint32_t timeout_ms);

void head_close(worker_t *w);
const char *head_state_name(worker_state_t s);

#endif
