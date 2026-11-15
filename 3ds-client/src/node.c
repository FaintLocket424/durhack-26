// Building for a console without its platform define would silently compile the PC path
#if defined(__powerpc__) && !defined(GEKKO)
#error "PowerPC build without -DGEKKO: check MACHDEP / CFLAGS in the Makefile"
#endif
#if defined(__arm__) && !defined(__3DS__)
#error "ARM build without -D__3DS__: check CFLAGS in the Makefile"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef GEKKO
#include <ogcsys.h>
#include <gccore.h>
#include <ogc/if_config.h>
#else
#include <poll.h>
#endif

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#define SOC_BUFFERSIZE 0x100000
static u32 *soc_buffer = NULL;
#endif

#include "node.h"
#include "protocol.h"
#include "handlers.h"

//---------------------------------------------------------------------------------
static inline bool keep_running(const node_t *node) {
//---------------------------------------------------------------------------------
#ifdef GEKKO
	return !node->stop && SYS_MainLoop();	// false on HOME / reset / power
#else
	// 3DS: aptMainLoop() belongs to the main thread, which sets node->stop.
	return !node->stop;
#endif
}

//---------------------------------------------------------------------------------
static void note_error(node_t *node, int code) {
//---------------------------------------------------------------------------------
	node->errors++;
	node->last_error = code;
}

//---------------------------------------------------------------------------------
// Block until sock is readable. Returns 1 = readable, 0 = asked to stop, -1 = error.
// On 3DS/PC this wakes every 250 ms to notice node->stop, so the thread can be
// joined and the soc service shut down cleanly (the Wii just blocks and relies
// on process exit).
//---------------------------------------------------------------------------------
static int wait_readable(int sock, const node_t *node) {
//---------------------------------------------------------------------------------
#ifdef GEKKO
	(void)sock; (void)node;
	return 1;
#else
	while (!node->stop) {
		struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
		int r = poll(&pfd, 1, 250);
		if (r > 0) return 1;
		if (r < 0 && errno != EINTR) return -1;
	}
	return 0;
#endif
}

//---------------------------------------------------------------------------------
// TCP is a byte stream: one recv()/send() may move less than asked for.
//---------------------------------------------------------------------------------
static int recv_all(const node_t *node, int sock, void *buf, size_t len) {
//---------------------------------------------------------------------------------
	uint8_t *p = buf;

	while (len > 0) {
		if (wait_readable(sock, node) <= 0) return -1;
		ssize_t n = recv(sock, p, len, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;				// 0: peer closed, <0: error
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
// Header and payload go out in a single send(). Two small back-to-back writes
// trigger Nagle + delayed ACK and add roughly 40 ms to every reply.
//---------------------------------------------------------------------------------
static int send_frame(int sock, uint16_t type, uint32_t request_id, const uint8_t *payload, uint32_t len) {
//---------------------------------------------------------------------------------
	frame_header_t h = { .type = type, .flags = 0, .request_id = request_id, .payload_len = len };

	uint8_t *frame = malloc(PROTO_HEADER_SIZE + len);
	if (frame == NULL) {
		// out of memory: slower but still correct
		uint8_t hdr[PROTO_HEADER_SIZE];
		proto_encode_header(hdr, &h);
		if (send_all(sock, hdr, sizeof hdr) < 0) return -1;
		return len ? send_all(sock, payload, len) : 0;
	}

	proto_encode_header(frame, &h);
	if (len) memcpy(frame + PROTO_HEADER_SIZE, payload, len);

	int ret = send_all(sock, frame, PROTO_HEADER_SIZE + len);
	free(frame);
	return ret;
}

//---------------------------------------------------------------------------------
static int send_error(int sock, uint32_t request_id, int status) {
//---------------------------------------------------------------------------------
	uint8_t code[4];
	put_u32(code, (uint32_t)-status);
	return send_frame(sock, MSG_ERROR, request_id, code, sizeof code);
}

//---------------------------------------------------------------------------------
// Serve one request. Returns 0 to keep the connection, -1 to drop it.
//---------------------------------------------------------------------------------
static int handle_request(int sock, node_t *node) {
//---------------------------------------------------------------------------------
	uint8_t hdr[PROTO_HEADER_SIZE];
	frame_header_t h;

	if (recv_all(node, sock, hdr, sizeof hdr) < 0) return -1;

	int status = proto_decode_header(hdr, &h);
	if (status != PROTO_OK) {
		// Bad magic or absurd length: the stream can't be trusted any more.
		note_error(node, status);
		send_error(sock, 0, status);
		return -1;
	}

	uint8_t *payload = NULL;
	if (h.payload_len > 0) {
		payload = malloc(h.payload_len);
		if (payload == NULL) {
			note_error(node, PROTO_ERR_NOMEM);
			send_error(sock, h.request_id, PROTO_ERR_NOMEM);
			return -1;
		}
		if (recv_all(node, sock, payload, h.payload_len) < 0) {
			free(payload);
			return -1;
		}
	}

	response_t resp;
	status = dispatch_request(h.type, payload, h.payload_len, &resp);
	free(payload);

	int ret;
	if (status == PROTO_OK) {
		ret = send_frame(sock, resp.type, h.request_id, resp.data, resp.len);
	} else {
		// A bad request is not fatal, the connection stays usable.
		note_error(node, status);
		ret = send_error(sock, h.request_id, status);
	}
	free(resp.data);

	node->requests++;
	return ret;
}

//---------------------------------------------------------------------------------
static int configure_network(node_t *node) {
//---------------------------------------------------------------------------------
#if defined(GEKKO)
	char localip[16] = {0};
	char gateway[16] = {0};
	char netmask[16] = {0};

	int ret = if_config(localip, netmask, gateway, true, 20);	// DHCP, 20 retries
	if (ret < 0) return ret;

	strncpy(node->local_ip, localip, sizeof node->local_ip - 1);

#elif defined(__3DS__)
	// The console does DHCP itself (system Wi-Fi settings); we only start the
	// socket service and wait until it reports an address.
	soc_buffer = memalign(0x1000, SOC_BUFFERSIZE);
	if (soc_buffer == NULL) return -ENOMEM;

	Result rc = socInit(soc_buffer, SOC_BUFFERSIZE);
	if (R_FAILED(rc)) {
		free(soc_buffer);
		soc_buffer = NULL;
		return (int)rc;						// negative, shown as last_error
	}

	in_addr_t ip = 0;
	for (int tries = 0; tries < 40 && !node->stop; tries++) {	// ~20 s
		ip = gethostid();
		if (ip != 0 && ip != htonl(INADDR_LOOPBACK)) break;
		ip = 0;
		usleep(500000);
	}
	if (ip == 0) return -ENETDOWN;			// no Wi-Fi / no address

	struct in_addr a = { .s_addr = ip };
	strncpy(node->local_ip, inet_ntoa(a), sizeof node->local_ip - 1);

#else
	strcpy(node->local_ip, "0.0.0.0");
#endif
	return 0;
}

//---------------------------------------------------------------------------------
static void shutdown_network(void) {
//---------------------------------------------------------------------------------
#ifdef __3DS__
	if (soc_buffer) {
		socExit();
		free(soc_buffer);
		soc_buffer = NULL;
	}
#endif
}

//---------------------------------------------------------------------------------
// Returns a listening socket, or a negative errno.
//---------------------------------------------------------------------------------
static int open_listener(uint16_t port) {
//---------------------------------------------------------------------------------
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		int err = errno;
		perror("socket");
		return -err;
	}

#if !defined(GEKKO) && !defined(__3DS__)
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif

	struct sockaddr_in server;
	memset(&server, 0, sizeof server);
	server.sin_family      = AF_INET;
	server.sin_port        = htons(port);
	server.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&server, sizeof server) != 0) {
		int err = errno;
		perror("bind");
		close(sock);
		return -err;
	}

	if (listen(sock, 1) != 0) {
		int err = errno;
		perror("listen");
		close(sock);
		return -err;
	}

	return sock;
}

//---------------------------------------------------------------------------------
static void *fail(node_t *node, int error) {
//---------------------------------------------------------------------------------
	node->last_error = error;
	node->state = NODE_FAILED;
	return NULL;
}

//---------------------------------------------------------------------------------
static void *node_main(node_t *node) {
//---------------------------------------------------------------------------------
	node->state = NODE_STARTING;

	int ret = configure_network(node);
	if (ret < 0) return fail(node, ret);

	int listener = open_listener(node->port);
	if (listener < 0) return fail(node, listener);

	while (keep_running(node)) {
		node->state = NODE_LISTENING;

		int w = wait_readable(listener, node);
		if (w == 0) break;					// asked to stop
		if (w < 0) {
			node->last_error = errno;
			usleep(100000);
			continue;
		}

		struct sockaddr_in client;
		socklen_t clientlen = sizeof client;
		int csock = accept(listener, (struct sockaddr *)&client, &clientlen);

		if (csock < 0) {
			node->last_error = errno;
			usleep(100000);					// don't spin if accept keeps failing
			continue;
		}

		node->state = NODE_CONNECTED;
		node->connections++;

		while (keep_running(node) && handle_request(csock, node) == 0) {
			// one request per iteration
		}

		close(csock);
	}

	close(listener);
	return NULL;
}

//---------------------------------------------------------------------------------
void *node_run(void *arg) {
//---------------------------------------------------------------------------------
	void *r = node_main(arg);
	shutdown_network();
	return r;
}
