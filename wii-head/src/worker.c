#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"
#include "sched.h"
#include "worker.h"

#define RECV_TIMEOUT_S	120				// a node that is silent this long is dropped
#define RESULT_BUF_SIZE	(TILE_RESPONSE_PREFIX + WORKER_MAX_PIXELS * 12)

//---------------------------------------------------------------------------------
static int recv_all(int sock, void *buf, size_t len) {
//---------------------------------------------------------------------------------
	uint8_t *p = buf;
	while (len > 0) {
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

//---------------------------------------------------------------------------------
static int connect_to(const char *ip, uint16_t port) {
//---------------------------------------------------------------------------------
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(port);
	if (inet_aton(ip, &addr.sin_addr) == 0 ||
	    connect(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
		close(sock);
		return -1;
	}

	// Best effort: if the stack ignores this, a hung node just blocks its thread.
	struct timeval tv = { RECV_TIMEOUT_S, 0 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	return sock;
}

//---------------------------------------------------------------------------------
// One request/response round trip. Returns 0, or -1 if the node is unusable.
//---------------------------------------------------------------------------------
static int do_tile(worker_t *w, int sock, uint32_t rid, const tile_t *t, uint8_t *buf) {
//---------------------------------------------------------------------------------
	image_t *img = w->img;

	// request: header + 9 x u32 in a single send()
	uint8_t req[PROTO_HEADER_SIZE + TILE_REQUEST_SIZE];
	frame_header_t h = { .type = MSG_RENDER_TILE, .flags = 0,
	                     .request_id = rid, .payload_len = TILE_REQUEST_SIZE };
	proto_encode_header(req, &h);

	uint8_t *p = req + PROTO_HEADER_SIZE;
	put_u32(p +  0, t->x0);
	put_u32(p +  4, t->y0);
	put_u32(p +  8, t->w);
	put_u32(p + 12, t->h);
	put_u32(p + 16, img->w);
	put_u32(p + 20, img->h);
	put_u32(p + 24, img->spp);
	put_u32(p + 28, t->seed);
	put_u32(p + 32, img->depth);

	if (send_all(sock, req, sizeof req) < 0) { w->last_error = -1; return -1; }

	// reply header
	uint8_t hdr[PROTO_HEADER_SIZE];
	frame_header_t r;
	if (recv_all(sock, hdr, sizeof hdr) < 0 ||
	    proto_decode_header(hdr, &r) != PROTO_OK ||
	    r.request_id != rid) { w->last_error = -1; return -1; }

	if (r.type == MSG_ERROR) {
		uint8_t c[4];
		w->last_error = (r.payload_len == 4 && recv_all(sock, c, 4) == 0) ? (int)get_u32(c) : -1;
		return -1;
	}

	uint32_t expect = TILE_RESPONSE_PREFIX + t->w * t->h * 3 * 4;
	if (r.type != MSG_TILE_RESULT || r.payload_len != expect ||
	    recv_all(sock, buf, expect) < 0) { w->last_error = -1; return -1; }

	if (get_u32(buf) != t->x0 || get_u32(buf + 4) != t->y0 ||
	    get_u32(buf + 8) != t->w || get_u32(buf + 12) != t->h ||
	    get_u32(buf + 16) != img->spp) { w->last_error = -1; return -1; }

	// accumulate (tiles from different passes can overlap, so lock)
	LWP_MutexLock(img->lock);
	for (uint32_t ty = 0; ty < t->h; ty++) {
		float         *dst = img->sum + ((t->y0 + ty) * img->w + t->x0) * 3;
		const uint8_t *src = buf + TILE_RESPONSE_PREFIX + ty * t->w * 12;
		for (uint32_t i = 0; i < t->w * 3; i++)
			dst[i] += get_f32(src + i * 4);
	}
	LWP_MutexUnlock(img->lock);
	return 0;
}

//---------------------------------------------------------------------------------
static void *worker_run(void *arg) {
//---------------------------------------------------------------------------------
	worker_t *w = arg;

	uint8_t *buf = malloc(RESULT_BUF_SIZE);
	int sock = buf ? connect_to(w->ip, w->port) : -1;
	if (sock < 0) {
		w->last_error = -2;
		free(buf);
		w->state = W_DEAD;
		return NULL;
	}

	uint32_t rid = 0;
	worker_state_t end = W_DONE;
	tile_t t;

	for (;;) {
		int r = sched_next(w->tile_w, w->tile_h, &t);
		if (r < 0) break;
		if (r == 0) {					// others still working, a tile may come back
			w->state = W_IDLE;
			usleep(50000);
			continue;
		}

		w->state = W_RENDERING;
		if (do_tile(w, sock, ++rid, &t, buf) < 0) {
			sched_requeue(&t);			// somebody else renders it
			end = W_DEAD;
			break;
		}
		sched_complete(&t);
		w->tiles++;
	}

	close(sock);						// node goes back to listening
	free(buf);
	w->state = end;						// last, so the UI never sees DONE while we still use buf
	return NULL;
}

//---------------------------------------------------------------------------------
int worker_start(worker_t *w) {
//---------------------------------------------------------------------------------
	w->tiles = 0;
	w->last_error = 0;

	if (w->tile_w == 0 || w->tile_h == 0 || w->tile_w > 4096 || w->tile_h > 4096 ||
	    w->tile_w * w->tile_h > WORKER_MAX_PIXELS) {
		w->last_error = -3;
		w->state = W_DEAD;
		return -1;
	}

	w->state = W_CONNECTING;
	if (LWP_CreateThread(&w->thread, worker_run, w, NULL, 32 * 1024, 50) != 0) {
		w->state = W_DEAD;
		return -1;
	}
	return 0;
}
