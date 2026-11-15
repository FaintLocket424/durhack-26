#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <gccore.h>

#include "coordinator.h"
#include "protocol.h"

// Outcome of one tile attempt
enum {
	RT_OK       =  0,
	RT_IO       = -1,		// connection broken or stream out of sync: reconnect
	RT_REJECTED = -2,		// worker answered MSG_ERROR, connection still fine
	RT_FATAL    = -3		// worker can't render at all (no handler)
};

//---------------------------------------------------------------------------------
// Socket helpers. TCP is a byte stream, so loop until everything has moved.
// Reads time out so a hung worker can't stall its thread forever.
//---------------------------------------------------------------------------------
static int wait_readable(int sock) {
//---------------------------------------------------------------------------------
	fd_set rfds;
	struct timeval tv;

	FD_ZERO(&rfds);
	FD_SET(sock, &rfds);
	tv.tv_sec  = RECV_TIMEOUT_S;
	tv.tv_usec = 0;
	return select(sock + 1, &rfds, NULL, NULL, &tv) > 0 ? 0 : -1;
}

//---------------------------------------------------------------------------------
static int recv_all(int sock, void *buf, size_t len) {
//---------------------------------------------------------------------------------
	uint8_t *p = buf;

	while (len > 0) {
		if (wait_readable(sock) < 0) return -1;
		ssize_t n = recv(sock, p, len, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

//---------------------------------------------------------------------------------
static int send_all(int sock, const void *buf, size_t len) {
//---------------------------------------------------------------------------------
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = send(sock, p, len, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

// Header and payload in one send() (avoids Nagle/delayed-ACK stalls, same as the worker).
//---------------------------------------------------------------------------------
static int send_frame(int sock, uint16_t type, uint32_t request_id, const uint8_t *payload, uint32_t len) {
//---------------------------------------------------------------------------------
	uint8_t frame[PROTO_HEADER_SIZE + TILE_REQUEST_SIZE];
	frame_header_t h = { .type = type, .flags = 0, .request_id = request_id, .payload_len = len };

	if (len > TILE_REQUEST_SIZE) return -1;
	proto_encode_header(frame, &h);
	if (len) memcpy(frame + PROTO_HEADER_SIZE, payload, len);
	return send_all(sock, frame, PROTO_HEADER_SIZE + len);
}

// Reads one frame; payload goes into buf (capacity cap).
//---------------------------------------------------------------------------------
static int read_frame(int sock, frame_header_t *h, uint8_t *buf, uint32_t cap) {
//---------------------------------------------------------------------------------
	uint8_t hdr[PROTO_HEADER_SIZE];

	if (recv_all(sock, hdr, sizeof hdr) < 0) return -1;
	if (proto_decode_header(hdr, h) != PROTO_OK) return -1;
	if (h->payload_len > cap) return -1;
	if (h->payload_len > 0 && recv_all(sock, buf, h->payload_len) < 0) return -1;
	return 0;
}

//---------------------------------------------------------------------------------
static uint32_t next_id(worker_t *w) {
//---------------------------------------------------------------------------------
	// worker index in the top byte keeps ids unique across workers (handy when debugging)
	return ((uint32_t)(w->index + 1) << 24) | (++w->seq & 0xFFFFFF);
}

//---------------------------------------------------------------------------------
// Connect and make sure the other end really is one of our nodes (PING/PONG).
//---------------------------------------------------------------------------------
static int worker_connect(worker_t *w) {
//---------------------------------------------------------------------------------
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		w->last_error = errno;
		return -1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(w->port);
	if (!inet_aton(w->ip, &addr.sin_addr)) {
		w->last_error = EINVAL;
		close(sock);
		return -1;
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
		w->last_error = errno;
		close(sock);
		return -1;
	}

	uint32_t id = next_id(w);
	frame_header_t h;
	if (send_frame(sock, MSG_PING, id, NULL, 0) < 0 ||
	    read_frame(sock, &h, w->buf, sizeof w->buf) < 0 ||
	    h.type != MSG_PONG || h.request_id != id) {
		w->last_error = PROTO_ERR_BAD_MAGIC;
		close(sock);
		return -1;
	}
	return sock;
}

//---------------------------------------------------------------------------------
static int claim_tile(coordinator_t *c, worker_t *w) {
//---------------------------------------------------------------------------------
	int found = -1;

	LWP_MutexLock(c->lock);
	for (int i = 0; i < N_TILES; i++) {
		if (c->tiles[i].state == TILE_PENDING) {
			c->tiles[i].state = TILE_ACTIVE;
			c->tiles[i].owner = (uint8_t)w->index;
			c->tiles[i].attempts++;
			found = i;
			break;
		}
	}
	LWP_MutexUnlock(c->lock);
	return found;
}

//---------------------------------------------------------------------------------
static void tile_finished(coordinator_t *c, worker_t *w, int idx) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(c->lock);
	c->tiles[idx].state = TILE_DONE;
	c->tiles_done++;
	LWP_MutexUnlock(c->lock);
	w->tiles_done++;
}

// Put a tile back in the pool. penalize=false for failures that aren't the tile's fault.
//---------------------------------------------------------------------------------
static void tile_release(coordinator_t *c, int idx, bool penalize) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(c->lock);
	tile_t *t = &c->tiles[idx];
	if (!penalize && t->attempts > 0) t->attempts--;
	if (t->attempts >= MAX_TILE_ATTEMPTS) {
		t->state = TILE_FAILED;
		c->tiles_failed++;
	} else {
		t->state = TILE_PENDING;
	}
	LWP_MutexUnlock(c->lock);
}

//---------------------------------------------------------------------------------
static int render_tile(worker_t *w, int sock, int idx) {
//---------------------------------------------------------------------------------
	coordinator_t *c = w->c;
	int tx, ty, tw, th;
	tile_rect(idx, &tx, &ty, &tw, &th);

	tile_request_t req = {
		.image_w = IMG_W, .image_h = IMG_H,
		.x = tx, .y = ty, .w = tw, .h = th,
		.p = c->params
	};
	uint8_t payload[TILE_REQUEST_SIZE];
	tile_encode_request(payload, &req);

	uint32_t id = next_id(w);
	if (send_frame(sock, MSG_RENDER_TILE, id, payload, sizeof payload) < 0) return RT_IO;

	frame_header_t h;
	if (read_frame(sock, &h, w->buf, sizeof w->buf) < 0) return RT_IO;
	if (h.request_id != id) return RT_IO;			// out of sync

	if (h.type == MSG_ERROR) {
		if (h.payload_len != 4) return RT_IO;
		int status = -(int)get_u32(w->buf);
		w->last_error = status;
		return status == PROTO_ERR_UNKNOWN_TYPE ? RT_FATAL : RT_REJECTED;
	}

	if (h.type != MSG_TILE_RESULT) return RT_IO;
	if (h.payload_len != (uint32_t)(tw * th * TILE_BYTES_PER_PIXEL)) return RT_IO;

	// Tiles never overlap, so no lock is needed for the pixel copy.
	for (int y = 0; y < th; y++) {
		memcpy(&c->pixels[((ty + y) * IMG_W + tx) * 3],
		       &w->buf[y * tw * 3], (size_t)tw * 3);
	}
	return RT_OK;
}

//---------------------------------------------------------------------------------
static void *worker_main(void *arg) {
//---------------------------------------------------------------------------------
	worker_t *w = arg;
	coordinator_t *c = w->c;
	int sock = -1;
	int fails = 0;

	while (!c->stop && c->tiles_done + c->tiles_failed < N_TILES) {

		if (sock < 0) {
			w->state = W_CONNECTING;
			sock = worker_connect(w);
			if (sock < 0) {
				if (++fails >= CONNECT_RETRIES) { w->state = W_DEAD; break; }
				usleep(500000);
				continue;
			}
			fails = 0;
		}

		int idx = claim_tile(c, w);
		if (idx < 0) {
			// Nothing pending, but tiles on other workers may still bounce back.
			w->state = W_IDLE;
			usleep(20000);
			continue;
		}

		w->state = W_BUSY;
		int r = render_tile(w, sock, idx);

		if (r == RT_OK) {
			tile_finished(c, w, idx);
		} else if (r == RT_REJECTED) {
			w->errors++;
			tile_release(c, idx, true);
		} else if (r == RT_FATAL) {
			w->errors++;
			tile_release(c, idx, false);
			w->state = W_DEAD;
			break;
		} else {
			w->errors++;
			tile_release(c, idx, true);
			close(sock);
			sock = -1;
		}
	}

	if (sock >= 0) close(sock);
	if (w->state != W_DEAD) w->state = W_DONE;

	LWP_MutexLock(c->lock);
	if (--c->workers_alive == 0) c->end_ticks = gettime();
	LWP_MutexUnlock(c->lock);
	return NULL;
}

//---------------------------------------------------------------------------------
void coordinator_init(coordinator_t *c, const char *const *ips, int n, uint16_t port, const render_params_t *params) {
//---------------------------------------------------------------------------------
	memset(c, 0, sizeof *c);
	LWP_MutexInit(&c->lock, false);
	c->params = *params;

	if (n > MAX_WORKERS) n = MAX_WORKERS;
	c->n_workers = n;
	for (int i = 0; i < n; i++) {
		worker_t *w = &c->workers[i];
		w->c = c;
		w->index = i;
		w->port = port;
		strncpy(w->ip, ips[i], sizeof w->ip - 1);
	}
}

//---------------------------------------------------------------------------------
void coordinator_start(coordinator_t *c) {
//---------------------------------------------------------------------------------
	memset(c->pixels, 0, sizeof c->pixels);
	memset(c->tiles, 0, sizeof c->tiles);			// all TILE_PENDING
	c->tiles_done = 0;
	c->tiles_failed = 0;
	c->stop = 0;
	c->workers_alive = c->n_workers;
	c->start_ticks = gettime();
	c->end_ticks = 0;

	for (int i = 0; i < c->n_workers; i++) {
		worker_t *w = &c->workers[i];
		w->state = W_CONNECTING;
		w->tiles_done = 0;
		w->errors = 0;
		w->last_error = 0;
		w->started = true;
		LWP_CreateThread(&w->thread, worker_main, w, NULL, 32 * 1024, 50);
	}
}

//---------------------------------------------------------------------------------
void coordinator_join(coordinator_t *c) {
//---------------------------------------------------------------------------------
	for (int i = 0; i < c->n_workers; i++) {
		if (c->workers[i].started) {
			LWP_JoinThread(c->workers[i].thread, NULL);
			c->workers[i].started = false;
		}
	}
}

//---------------------------------------------------------------------------------
void coordinator_stop(coordinator_t *c) {
//---------------------------------------------------------------------------------
	c->stop = 1;
}

//---------------------------------------------------------------------------------
bool coordinator_done(coordinator_t *c) {
//---------------------------------------------------------------------------------
	return c->workers_alive == 0;
}

//---------------------------------------------------------------------------------
tile_state_t coordinator_tile(coordinator_t *c, int idx, int *owner) {
//---------------------------------------------------------------------------------
	LWP_MutexLock(c->lock);
	tile_state_t st = (tile_state_t)c->tiles[idx].state;
	*owner = c->tiles[idx].owner;
	LWP_MutexUnlock(c->lock);
	return st;
}

//---------------------------------------------------------------------------------
uint32_t coordinator_elapsed_ms(coordinator_t *c) {
//---------------------------------------------------------------------------------
	uint64_t end = c->end_ticks ? c->end_ticks : gettime();
	return (uint32_t)ticks_to_millisecs(end - c->start_ticks);
}
