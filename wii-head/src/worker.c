#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#if defined(__has_include)
#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>
#endif
#endif
#include <ogc/lwp_watchdog.h>

#include "protocol.h"
#include "sched.h"
#include "worker.h"
#include "log.h"

#define RECV_TIMEOUT_S	120				// a node that is silent this long is dropped
#define RESULT_BUF_SIZE	(TILE_RESPONSE_PREFIX + WORKER_MAX_PIXELS * 12)
#define DEFAULT_PIPE	2

// Where an I/O call failed, for the error text.
typedef struct {
	int      rc;						// return value of the failing recv/send/connect
	int      err;						// errno right after it
	uint32_t got, want;					// bytes moved before the failure / bytes asked for
} io_fail_t;

typedef struct {
	tile_t   t;
	uint32_t rid;
	uint32_t sent_ms;
} pending_t;

//---------------------------------------------------------------------------------
uint32_t worker_now_ms(void) {
//---------------------------------------------------------------------------------
	return (uint32_t)ticks_to_millisecs(gettime());
}

//---------------------------------------------------------------------------------
static const char *errno_name(int e) {
//---------------------------------------------------------------------------------
	if (e < 0) e = -e;
	switch (e) {
		case 0:             return "";
		case ENOMEM:        return "ENOMEM";
		case ETIMEDOUT:     return "ETIMEDOUT";
		case ECONNRESET:    return "ECONNRESET";
		case ECONNREFUSED:  return "ECONNREFUSED";
		case ECONNABORTED:  return "ECONNABORTED";
		case EHOSTUNREACH:  return "EHOSTUNREACH";
		case ENETUNREACH:   return "ENETUNREACH";
		case ENETDOWN:      return "ENETDOWN";
		case EPIPE:         return "EPIPE";
		case EAGAIN:        return "EAGAIN";
		case ENOTCONN:      return "ENOTCONN";
		case EINVAL:        return "EINVAL";
		default:            return "errno";
	}
}

//---------------------------------------------------------------------------------
static const char *proto_name(int status) {
//---------------------------------------------------------------------------------
	if (status > 0) status = -status;	// MSG_ERROR carries -(proto_status_t)
	switch (status) {
		case PROTO_ERR_BAD_MAGIC:    return "BAD_MAGIC";
		case PROTO_ERR_TOO_LARGE:    return "TOO_LARGE";
		case PROTO_ERR_UNKNOWN_TYPE: return "UNKNOWN_TYPE";
		case PROTO_ERR_BAD_PAYLOAD:  return "BAD_PAYLOAD";
		case PROTO_ERR_NOMEM:        return "NODE_NOMEM";
		case PROTO_ERR_ABORTED:      return "ABORTED(node quit)";
		default:                     return "?";
	}
}

//---------------------------------------------------------------------------------
static void set_err(worker_t *w, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(worker_t *w, int code, const char *fmt, ...) {
//---------------------------------------------------------------------------------
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(w->err, sizeof w->err, fmt, ap);
	va_end(ap);
	w->last_error = code;
}

// "recv hdr: rc=0 closed 0/16B" / "send req: rc=-1 ECONNRESET(104) 0/52B"
//---------------------------------------------------------------------------------
static void set_io_err(worker_t *w, const char *what, const io_fail_t *f) {
//---------------------------------------------------------------------------------
	if (f->rc == 0)
		set_err(w, -1, "%s: closed %u/%uB", what, (unsigned)f->got, (unsigned)f->want);
	else if (f->err)
		set_err(w, f->err, "%s: %s(%d) %u/%uB", what, errno_name(f->err), f->err,
		        (unsigned)f->got, (unsigned)f->want);
	else
		set_err(w, f->rc, "%s: rc=%d %u/%uB", what, f->rc, (unsigned)f->got, (unsigned)f->want);
}

//---------------------------------------------------------------------------------
static int recv_all(worker_t *w, int sock, void *buf, uint32_t len, io_fail_t *f) {
//---------------------------------------------------------------------------------
	uint8_t *p = buf;
	uint32_t got = 0;
	while (got < len) {
		uint32_t want = len - got;
		ssize_t n = recv(sock, p + got, want > NET_CHUNK ? NET_CHUNK : want, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			f->rc = (int)n; f->err = n < 0 ? errno : 0; f->got = got; f->want = len;
			return -1;
		}
		got += (uint32_t)n;
		w->last_rx_ms = worker_now_ms();
	}
	return 0;
}

//---------------------------------------------------------------------------------
static int send_all(worker_t *w, int sock, const void *buf, uint32_t len, io_fail_t *f) {
//---------------------------------------------------------------------------------
	const uint8_t *p = buf;
	uint32_t sent = 0;
	while (sent < len) {
		uint32_t want = len - sent;
		ssize_t n = send(sock, p + sent, want > NET_CHUNK ? NET_CHUNK : want, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			f->rc = (int)n; f->err = n < 0 ? errno : 0; f->got = sent; f->want = len;
			return -1;
		}
		sent += (uint32_t)n;
	}
	(void)w;
	return 0;
}

//---------------------------------------------------------------------------------
static int connect_to(worker_t *w) {
//---------------------------------------------------------------------------------
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		set_err(w, errno, "socket: rc=%d %s(%d)", sock, errno_name(errno), errno);
		return -1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(w->port);
	if (inet_aton(w->ip, &addr.sin_addr) == 0) {
		set_err(w, -2, "bad ip in config.h");
		close(sock);
		return -1;
	}
	int rc = connect(sock, (struct sockaddr *)&addr, sizeof addr);
	if (rc != 0) {
		int e = errno;
		set_err(w, e ? e : rc, "connect: rc=%d %s(%d)", rc, errno_name(e ? e : rc), e);
		close(sock);
		return -1;
	}

	// Best effort: if the stack ignores this, a hung node just blocks its thread
	// (the UI shows how long it has been quiet).
	struct timeval tv = { RECV_TIMEOUT_S, 0 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef TCP_NODELAY
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#endif
	return sock;
}

//---------------------------------------------------------------------------------
static int send_request(worker_t *w, int sock, const pending_t *pd) {
//---------------------------------------------------------------------------------
	image_t *img = w->img;
	const tile_t *t = &pd->t;

	// header + 9 x u32 in a single send()
	uint8_t req[PROTO_HEADER_SIZE + TILE_REQUEST_SIZE];
	frame_header_t h = { .type = MSG_RENDER_TILE, .flags = w->no_f16 ? 0 : FLAG_F16_RESULT,
	                     .request_id = pd->rid, .payload_len = TILE_REQUEST_SIZE };
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

	io_fail_t f;
	if (send_all(w, sock, req, sizeof req, &f) < 0) {
		set_io_err(w, "send req", &f);
		return -1;
	}
	return 0;
}

//---------------------------------------------------------------------------------
// Read and accumulate the reply for pd. free_ms: when the node could start on
// this tile (it was sent, and the node had finished the one before it).
//---------------------------------------------------------------------------------
static int recv_result(worker_t *w, int sock, const pending_t *pd, uint8_t *buf, uint32_t free_ms) {
//---------------------------------------------------------------------------------
	image_t *img = w->img;
	const tile_t *t = &pd->t;
	io_fail_t f;

	uint8_t hdr[PROTO_HEADER_SIZE];
	if (recv_all(w, sock, hdr, sizeof hdr, &f) < 0) {
		set_io_err(w, "recv hdr", &f);
		return -1;
	}
	uint32_t hdr_ms = worker_now_ms();

	frame_header_t r;
	int st = proto_decode_header(hdr, &r);
	if (st != PROTO_OK) {
		set_err(w, st, "bad reply hdr: %s magic=%08x", proto_name(st), (unsigned)get_u32(hdr));
		return -1;
	}
	if (r.request_id != pd->rid) {
		set_err(w, -1, "reply id %u, expected %u", (unsigned)r.request_id, (unsigned)pd->rid);
		return -1;
	}

	if (r.type == MSG_ERROR) {
		uint8_t c[4];
		if (r.payload_len == 4 && recv_all(w, sock, c, 4, &f) == 0) {
			int code = (int)get_u32(c);
			set_err(w, code, "node error %d %s", code, proto_name(code));
		} else {
			set_err(w, -1, "node error, unreadable code");
		}
		return -1;
	}

	int      half   = (r.flags & FLAG_F16_RESULT) != 0;
	uint32_t esize  = half ? 2 : 4;
	uint32_t expect = TILE_RESPONSE_PREFIX + t->w * t->h * 3 * esize;
	if (r.type != MSG_TILE_RESULT || r.payload_len != expect) {
		set_err(w, -1, "reply type %04x len %u, want %u", r.type, (unsigned)r.payload_len, (unsigned)expect);
		return -1;
	}
	if (recv_all(w, sock, buf, expect, &f) < 0) {
		set_io_err(w, "recv px", &f);
		return -1;
	}
	uint32_t done_ms = worker_now_ms();

	if (get_u32(buf) != t->x0 || get_u32(buf + 4) != t->y0 ||
	    get_u32(buf + 8) != t->w || get_u32(buf + 12) != t->h ||
	    get_u32(buf + 16) != img->spp) {
		set_err(w, -1, "echo mismatch: got %u,%u %ux%u", (unsigned)get_u32(buf), (unsigned)get_u32(buf + 4),
		        (unsigned)get_u32(buf + 8), (unsigned)get_u32(buf + 12));
		return -1;
	}

	// accumulate (tiles from different passes can overlap, so lock)
	const uint8_t *src = buf + TILE_RESPONSE_PREFIX;
	LWP_MutexLock(img->lock);
	for (uint32_t ty = 0; ty < t->h; ty++) {
		float *dst = img->sum + ((t->y0 + ty) * img->w + t->x0) * 3;
		if (half) {
			for (uint32_t i = 0; i < t->w * 3; i++, src += 2)
				dst[i] += f16_to_f32(get_u16(src));
		} else {
			for (uint32_t i = 0; i < t->w * 3; i++, src += 4)
				dst[i] += get_f32(src);
		}
	}
	LWP_MutexUnlock(img->lock);

	// stats
	uint32_t node_ms = hdr_ms - free_ms;
	uint32_t xfer_ms = done_ms - hdr_ms;
	w->node_ms        = node_ms;
	w->xfer_ms        = xfer_ms;
	w->node_ms_total += node_ms;
	w->px_done       += t->w * t->h;
	w->f16            = (uint32_t)half;
	if (xfer_ms > 0) {
		uint32_t kbps = expect / xfer_ms;	// bytes per ms ~= KB/s
		w->kbps = w->kbps ? (w->kbps * 3 + kbps) / 4 : kbps;
	}
	return 0;
}

//---------------------------------------------------------------------------------
// One connection. Returns 1 when all work is finished, 0 when the connection
// failed (every queued tile has been handed back to the scheduler).
//---------------------------------------------------------------------------------
static int run_session(worker_t *w, int sock, uint8_t *buf) {
//---------------------------------------------------------------------------------
	pending_t q[WORKER_PIPE_MAX];
	uint32_t  head = 0, n = 0, rid = 0;
	uint32_t  depth = w->pipeline ? w->pipeline : DEFAULT_PIPE;
	if (depth > WORKER_PIPE_MAX) depth = WORKER_PIPE_MAX;
	uint32_t  node_free_ms = worker_now_ms();	// when the node finished its last tile

	for (;;) {
		// keep the node's queue full
		int r = 1;
		while (n < depth) {
			pending_t *pd = &q[(head + n) % WORKER_PIPE_MAX];
			r = sched_next(w->tile_w, w->tile_h, &pd->t);
			if (r != 1) break;
			pd->rid = ++rid;
			pd->sent_ms = worker_now_ms();
			n++;							// count it first so a failed send requeues it
			w->in_flight = n;
			if (send_request(w, sock, pd) < 0) goto fail;
		}
		w->in_flight = n;

		if (n == 0) {
			if (r < 0) return 1;			// everything is done
			w->state = W_IDLE;				// others still busy, a tile may come back
			usleep(50000);
			continue;
		}

		// replies come back in request order
		pending_t *pd = &q[head];
		w->state = W_RENDERING;
		w->cur_x = pd->t.x0; w->cur_y = pd->t.y0;
		w->cur_w = pd->t.w;  w->cur_h = pd->t.h;

		uint32_t free_ms = (int32_t)(pd->sent_ms - node_free_ms) > 0 ? pd->sent_ms : node_free_ms;
		if (recv_result(w, sock, pd, buf, free_ms) < 0) goto fail;

		node_free_ms = worker_now_ms();
		sched_complete(&pd->t);
		w->tiles++;
		w->fail_streak = 0;
		head = (head + 1) % WORKER_PIPE_MAX;
		n--;
	}

fail:
	for (uint32_t i = 0; i < n; i++)
		sched_requeue(&q[(head + i) % WORKER_PIPE_MAX].t);
	log_event("%s: %s, %u requeued", w->ip, w->err, (unsigned)n);
	w->in_flight = 0;
	return 0;
}

//---------------------------------------------------------------------------------
static void *worker_run(void *arg) {
//---------------------------------------------------------------------------------
	worker_t *w = arg;
	worker_state_t end = W_DEAD;

	uint8_t *buf = malloc(RESULT_BUF_SIZE);
	if (buf == NULL) {
		set_err(w, ENOMEM, "head out of memory");
		log_event("%s %s", w->ip, w->err);
		w->state = W_DEAD;
		return NULL;
	}

	while (!sched_finished()) {
		w->conn_start_ms = worker_now_ms();
		w->state = W_CONNECTING;
		int sock = connect_to(w);

		if (sock >= 0) {
			w->connects++;
			w->last_rx_ms = worker_now_ms();
			log_event("%s connected%s", w->ip, w->connects > 1 ? " (reconnect)" : "");

			int done = run_session(w, sock, buf);
			close(sock);					// node goes back to listening
			if (done) { end = W_DONE; break; }
		} else {
			log_event("%s %s", w->ip, w->err);
		}

		w->failures++;
		if (++w->fail_streak >= WORKER_MAX_FAILS) {
			log_event("%s GIVING UP after %u failures in a row", w->ip, (unsigned)w->fail_streak);
			break;
		}

		// 0.5s, 1s, 2s, 4s ... between attempts
		w->state = W_BACKOFF;
		uint32_t wait_ms = 500u << (w->fail_streak - 1);
		if (wait_ms > 5000) wait_ms = 5000;
		usleep(wait_ms * 1000);
	}
	if (sched_finished() && end != W_DONE) end = W_DONE;

	free(buf);
	w->in_flight = 0;
	w->state = end;						// last, so the UI never sees DONE while we still use buf
	return NULL;
}

//---------------------------------------------------------------------------------
int worker_start(worker_t *w) {
//---------------------------------------------------------------------------------
	w->tiles = 0;
	w->last_error = 0;
	w->err[0] = '\0';

	if (w->tile_w == 0 || w->tile_h == 0 || w->tile_w > 4096 || w->tile_h > 4096 ||
	    w->tile_w * w->tile_h > WORKER_MAX_PIXELS) {
		set_err(w, -3, "bad tile size in config.h");
		log_event("%s %s", w->ip, w->err);
		w->state = W_DEAD;
		return -1;
	}

	w->state = W_CONNECTING;
	if (LWP_CreateThread(&w->thread, worker_run, w, NULL, 64 * 1024, 50) != 0) {
		set_err(w, -4, "could not start thread");
		log_event("%s %s", w->ip, w->err);
		w->state = W_DEAD;
		return -1;
	}
	return 0;
}
