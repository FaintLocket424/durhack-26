// plat_listen/accept/recv/send over BSD sockets.
// Shared by: PC (posix), Wii (libogc), Wii U (wut), DS (dswifi).
// Define PLAT_NET_POLLED to use select() timeouts so a single-threaded target
// gets PLAT_AGAIN (and thus plat_idle()) instead of blocking forever.
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef PLAT_NET_POLLED
#include <sys/select.h>
#endif

#include "platform.h"

static int listener = -1;

#ifdef PLAT_NET_POLLED
static int ready(int fd, int for_write) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(fd, &s);
	struct timeval tv = { 0, 50000 };		// 50 ms
	int r = select(fd + 1, for_write ? NULL : &s, for_write ? &s : NULL, NULL, &tv);
	return r > 0;
}
#endif

int plat_listen(uint16_t port) {
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) return -errno;

#if !defined(GEKKO) && !defined(NDS) && !defined(__WIIU__)
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif

	struct sockaddr_in server;
	memset(&server, 0, sizeof server);
	server.sin_family      = AF_INET;
	server.sin_port        = htons(port);
	server.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&server, sizeof server) != 0 || listen(sock, 1) != 0) {
		int err = errno;
		close(sock);
		return -err;
	}
	listener = sock;
	return 0;
}

void plat_unlisten(void) {
	if (listener >= 0) close(listener);
	listener = -1;
}

int plat_accept(void) {
#ifdef PLAT_NET_POLLED
	if (!ready(listener, 0)) return PLAT_AGAIN;
#endif
	struct sockaddr_in client;
	socklen_t len = sizeof client;
	int c = accept(listener, (struct sockaddr *)&client, &len);
	return c < 0 ? -errno : c;
}

int plat_recv(plat_conn_t c, void *buf, size_t len) {
#ifdef PLAT_NET_POLLED
	if (!ready(c, 0)) return PLAT_AGAIN;
#endif
	ssize_t n = recv(c, buf, len, 0);
	if (n < 0 && errno == EINTR) return PLAT_AGAIN;
	return n > 0 ? (int)n : -1;
}

int plat_send(plat_conn_t c, const void *buf, size_t len) {
#ifdef PLAT_NET_POLLED
	if (!ready(c, 1)) return PLAT_AGAIN;
#endif
	ssize_t n = send(c, buf, len, 0);
	if (n < 0 && errno == EINTR) return PLAT_AGAIN;
	return n > 0 ? (int)n : -1;
}

void plat_close(plat_conn_t c) { close(c); }
