// Building for the Wii without -DGEKKO would silently compile the PC code path
#if defined(__powerpc__) && !defined(GEKKO)
#error "PowerPC build without -DGEKKO: check MACHDEP / CFLAGS in the Makefile"
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef GEKKO
#include <ogcsys.h>
#include <gccore.h>
#endif

#include "head.h"

//---------------------------------------------------------------------------------
static uint64_t now_ms(void) {
//---------------------------------------------------------------------------------
#ifdef GEKKO
	return (uint64_t)ticks_to_millisecs(gettime());
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static inline int would_block(int e) {
	return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY;
}

//---------------------------------------------------------------------------------
static int set_nonblocking(int sock) {
//---------------------------------------------------------------------------------
	// libogc maps fcntl(F_SETFL, O_NONBLOCK) onto its own net_fcntl. If your
	// libogc version rejects it, use: u32 one = 1; net_ioctl(sock, FIONBIO, &one);
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0) return -1;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

//---------------------------------------------------------------------------------
void head_worker_init(worker_t *w, const char *ip, uint16_t port) {
//---------------------------------------------------------------------------------
	memset(w, 0, sizeof *w);
	strncpy(w->ip, ip, sizeof w->ip - 1);
	w->port  = port;
	w->sock  = -1;
	w->state = WORKER_IDLE;
}

//---------------------------------------------------------------------------------
void head_close(worker_t *w) {
//---------------------------------------------------------------------------------
	if (w->sock >= 0) close(w->sock);
	w->sock = -1;
}

//---------------------------------------------------------------------------------
static void fail(worker_t *w, int error) {
//---------------------------------------------------------------------------------
	w->error = error;
	w->state = WORKER_FAILED;
	head_close(w);			// a half-read stream can't be trusted, drop it
}

static inline int is_active(const worker_t *w) {
	return w->state >= WORKER_CONNECTING && w->state <= WORKER_RECV_PAYLOAD;
}

//---------------------------------------------------------------------------------
// Build the request and start connecting (or reuse an open connection).
//---------------------------------------------------------------------------------
static void start_ping(worker_t *w, const uint8_t *payload, uint32_t len) {
//---------------------------------------------------------------------------------
	w->error  = 0;
	w->rtt_ms = 0;
	w->rx_off = 0;

	frame_header_t h = { .type = MSG_PING, .flags = 0, .request_id = ++w->next_id, .payload_len = len };
	proto_encode_header(w->tx, &h);
	if (len) memcpy(w->tx + PROTO_HEADER_SIZE, payload, len);
	w->tx_len = PROTO_HEADER_SIZE + len;
	w->tx_off = 0;
	w->t_start = now_ms();

	if (w->sock >= 0) {			// reuse the open connection
		w->state = WORKER_SENDING;
		return;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(w->port);
	if (inet_aton(w->ip, &addr.sin_addr) == 0) { fail(w, EINVAL); return; }

	w->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (w->sock < 0) { fail(w, errno); return; }

	if (set_nonblocking(w->sock) < 0) { fail(w, errno); return; }

	if (connect(w->sock, (struct sockaddr *)&addr, sizeof addr) < 0 && !would_block(errno)) {
		fail(w, errno);
		return;
	}
	w->state = WORKER_CONNECTING;	// writable == connected (or failed, send() will tell us)
}

//---------------------------------------------------------------------------------
// Socket became writable: push as much of the request as the stack will take.
//---------------------------------------------------------------------------------
static void pump_send(worker_t *w) {
//---------------------------------------------------------------------------------
	w->state = WORKER_SENDING;

	while (w->tx_off < w->tx_len) {
		ssize_t n = send(w->sock, w->tx + w->tx_off, w->tx_len - w->tx_off, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			if (would_block(errno)) return;		// try again when writable
			fail(w, errno);						// includes refused connections
			return;
		}
		if (n == 0) { fail(w, EPIPE); return; }
		w->tx_off += (uint32_t)n;
	}

	w->rx_off = 0;
	w->state = WORKER_RECV_HEADER;
}

//---------------------------------------------------------------------------------
// Socket became readable: advance the incremental header/payload parser.
//---------------------------------------------------------------------------------
static void pump_recv(worker_t *w) {
//---------------------------------------------------------------------------------
	for (;;) {
		uint8_t *dst;
		uint32_t want;

		if (w->state == WORKER_RECV_HEADER) {
			dst  = w->rx_hdr + w->rx_off;
			want = PROTO_HEADER_SIZE - w->rx_off;
		} else {
			dst  = w->rx_payload + w->rx_off;
			want = w->rx_h.payload_len - w->rx_off;
		}

		if (want > 0) {
			ssize_t n = recv(w->sock, dst, want, 0);
			if (n < 0) {
				if (errno == EINTR) continue;
				if (would_block(errno)) return;
				fail(w, errno);
				return;
			}
			if (n == 0) { fail(w, ECONNRESET); return; }	// peer closed
			w->rx_off += (uint32_t)n;
			if ((uint32_t)n < want) continue;				// keep draining until EAGAIN
		}

		if (w->state == WORKER_RECV_HEADER) {
			int st = proto_decode_header(w->rx_hdr, &w->rx_h);
			if (st != PROTO_OK) { fail(w, st); return; }

			// Strictly validate before trusting anything in the frame.
			if (w->rx_h.request_id != w->next_id) { fail(w, PROTO_ERR_BAD_PAYLOAD); return; }
			if (w->rx_h.type == MSG_ERROR)         { fail(w, PROTO_ERR_UNKNOWN_TYPE); return; }
			if (w->rx_h.type != MSG_PONG)          { fail(w, PROTO_ERR_UNKNOWN_TYPE); return; }
			if (w->rx_h.payload_len != w->tx_len - PROTO_HEADER_SIZE) {
				fail(w, PROTO_ERR_BAD_PAYLOAD);			// also guards rx_payload size
				return;
			}
			w->rx_off = 0;
			w->state = WORKER_RECV_PAYLOAD;
			continue;
		}

		// payload complete: a PONG must echo the ping payload
		if (w->rx_h.payload_len &&
		    memcmp(w->rx_payload, w->tx + PROTO_HEADER_SIZE, w->rx_h.payload_len) != 0) {
			fail(w, PROTO_ERR_BAD_PAYLOAD);
			return;
		}
		w->rtt_ms = (uint32_t)(now_ms() - w->t_start);
		w->state = WORKER_OK;
		return;
	}
}

//---------------------------------------------------------------------------------
int head_ping(worker_t *w, int n, const uint8_t *payload, uint32_t len, uint32_t timeout_ms) {
//---------------------------------------------------------------------------------
	if (n < 0 || n > HEAD_MAX_WORKERS || len > HEAD_PING_MAX_PAYLOAD) return -EINVAL;
	if (len && payload == NULL) return -EINVAL;

	for (int i = 0; i < n; i++) start_ping(&w[i], payload, len);

	uint64_t deadline = now_ms() + timeout_ms;

	for (;;) {
		fd_set rd, wr;
		FD_ZERO(&rd);
		FD_ZERO(&wr);
		int maxfd = -1, active = 0;

		for (int i = 0; i < n; i++) {
			if (!is_active(&w[i])) continue;
			active++;
			if (w[i].state == WORKER_CONNECTING || w[i].state == WORKER_SENDING)
				FD_SET(w[i].sock, &wr);
			else
				FD_SET(w[i].sock, &rd);
			if (w[i].sock > maxfd) maxfd = w[i].sock;
		}
		if (active == 0) break;

		uint64_t now = now_ms();
		if (now >= deadline) break;
		uint64_t left = deadline - now;

		struct timeval tv = { .tv_sec = (long)(left / 1000), .tv_usec = (long)(left % 1000) * 1000 };
		int r = select(maxfd + 1, &rd, &wr, NULL, &tv);

		if (r < 0) {
			if (errno == EINTR) continue;
			for (int i = 0; i < n; i++) if (is_active(&w[i])) fail(&w[i], errno);
			break;
		}
		if (r == 0) break;			// timed out, handled below

		for (int i = 0; i < n; i++) {
			if (!is_active(&w[i])) continue;
			if (FD_ISSET(w[i].sock, &wr)) pump_send(&w[i]);
			// the send may have completed; the reply can only arrive after select says so
			if (is_active(&w[i]) && w[i].sock >= 0 && FD_ISSET(w[i].sock, &rd)) pump_recv(&w[i]);
		}
	}

	int ok = 0;
	for (int i = 0; i < n; i++) {
		if (is_active(&w[i])) {
			w[i].error = ETIMEDOUT;
			w[i].state = WORKER_TIMEOUT;
			head_close(&w[i]);		// a late reply would desync the stream
		}
		if (w[i].state == WORKER_OK) ok++;
	}
	return ok;
}

//---------------------------------------------------------------------------------
const char *head_state_name(worker_state_t s) {
//---------------------------------------------------------------------------------
	switch (s) {
		case WORKER_IDLE:         return "idle";
		case WORKER_CONNECTING:   return "connecting";
		case WORKER_SENDING:      return "sending";
		case WORKER_RECV_HEADER:  return "waiting";
		case WORKER_RECV_PAYLOAD: return "receiving";
		case WORKER_OK:           return "OK";
		case WORKER_FAILED:       return "FAILED";
		default:                  return "TIMEOUT";
	}
}
