// Portable TCP front end. No OS headers: everything goes through platform.h.
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "platform.h"
#include "protocol.h"
#include "handlers.h"

//---------------------------------------------------------------------------------
static inline bool keep_running(const node_t *node) {
//---------------------------------------------------------------------------------
	return !node->stop && plat_keep_running();
}

//---------------------------------------------------------------------------------
static void note_error(node_t *node, int code) {
//---------------------------------------------------------------------------------
	node->errors++;
	node->last_error = code;
}

//---------------------------------------------------------------------------------
// TCP is a byte stream: one recv()/send() may move less than asked for.
//---------------------------------------------------------------------------------
static int recv_all(node_t *node, plat_conn_t c, void *buf, size_t len) {
//---------------------------------------------------------------------------------
	uint8_t *p = buf;

	while (len > 0) {
		int n = plat_recv(c, p, len);
		if (n == PLAT_AGAIN) {
			if (!keep_running(node)) return -1;
			plat_idle();
			continue;
		}
		if (n <= 0) return -1;				// closed or error
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

//---------------------------------------------------------------------------------
static int send_all(node_t *node, plat_conn_t c, const void *buf, size_t len) {
//---------------------------------------------------------------------------------
	const uint8_t *p = buf;

	while (len > 0) {
		int n = plat_send(c, p, len);
		if (n == PLAT_AGAIN) {
			if (!keep_running(node)) return -1;
			plat_idle();
			continue;
		}
		if (n <= 0) return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

//---------------------------------------------------------------------------------
// Header and payload go out in a single send. Two small back-to-back writes
// trigger Nagle + delayed ACK and add roughly 40 ms to every reply.
//---------------------------------------------------------------------------------
static int send_frame(node_t *node, plat_conn_t c, uint16_t type, uint32_t request_id,
                      const uint8_t *payload, uint32_t len) {
//---------------------------------------------------------------------------------
	frame_header_t h = { .type = type, .flags = 0, .request_id = request_id, .payload_len = len };

	uint8_t *frame = malloc(PROTO_HEADER_SIZE + len);
	if (frame == NULL) {					// out of memory: slower but still correct
		uint8_t hdr[PROTO_HEADER_SIZE];
		proto_encode_header(hdr, &h);
		if (send_all(node, c, hdr, sizeof hdr) < 0) return -1;
		return len ? send_all(node, c, payload, len) : 0;
	}

	proto_encode_header(frame, &h);
	if (len) memcpy(frame + PROTO_HEADER_SIZE, payload, len);

	int ret = send_all(node, c, frame, PROTO_HEADER_SIZE + len);
	free(frame);
	return ret;
}

//---------------------------------------------------------------------------------
static int send_error(node_t *node, plat_conn_t c, uint32_t request_id, int status) {
//---------------------------------------------------------------------------------
	uint8_t code[4];
	put_u32(code, (uint32_t)-status);
	return send_frame(node, c, MSG_ERROR, request_id, code, sizeof code);
}

//---------------------------------------------------------------------------------
// Serve one request. Returns 0 to keep the connection, -1 to drop it.
//---------------------------------------------------------------------------------
static int handle_request(plat_conn_t c, node_t *node) {
//---------------------------------------------------------------------------------
	uint8_t hdr[PROTO_HEADER_SIZE];
	frame_header_t h;

	if (recv_all(node, c, hdr, sizeof hdr) < 0) return -1;

	int status = proto_decode_header(hdr, &h);
	if (status != PROTO_OK) {
		note_error(node, status);			// stream can't be trusted any more
		send_error(node, c, 0, status);
		return -1;
	}

	uint8_t *payload = NULL;
	if (h.payload_len > 0) {
		payload = malloc(h.payload_len);
		if (payload == NULL) {
			note_error(node, PROTO_ERR_NOMEM);
			send_error(node, c, h.request_id, PROTO_ERR_NOMEM);
			return -1;
		}
		if (recv_all(node, c, payload, h.payload_len) < 0) {
			free(payload);
			return -1;
		}
	}

	response_t resp;
	status = dispatch_request(h.type, payload, h.payload_len, &resp);
	free(payload);

	int ret;
	if (status == PROTO_OK) {
		ret = send_frame(node, c, resp.type, h.request_id, resp.data, resp.len);
	} else {								// bad request: connection stays usable
		note_error(node, status);
		ret = send_error(node, c, h.request_id, status);
	}
	free(resp.data);

	node->requests++;
	return ret;
}

// Renderer -> platform bridge: lets the target draw a preview and/or abort.
//---------------------------------------------------------------------------------
static int on_progress(const tile_job_t *job, const float *accum, uint32_t spp_done, void *user) {
//---------------------------------------------------------------------------------
	node_t *node = user;
	int abort = plat_preview(job, accum, spp_done);
	return abort || !keep_running(node);
}

//---------------------------------------------------------------------------------
static void *fail(node_t *node, int error) {
//---------------------------------------------------------------------------------
	node->last_error = error;
	node->state = NODE_FAILED;
	return NULL;
}

//---------------------------------------------------------------------------------
void *node_run(void *arg) {
//---------------------------------------------------------------------------------
	node_t *node = arg;

	node->state = NODE_STARTING;
	render_set_progress(on_progress, node);

	int ret = plat_net_init(node->local_ip, sizeof node->local_ip);
	if (ret < 0) return fail(node, ret);

	ret = plat_listen(node->port);
	if (ret < 0) return fail(node, ret);

	while (keep_running(node)) {
		node->state = NODE_LISTENING;

		int c = plat_accept();
		if (c == PLAT_AGAIN) { plat_idle(); continue; }
		if (c < 0) {
			node->last_error = c;
			plat_sleep_ms(100);				// don't spin if accept keeps failing
			continue;
		}

		node->state = NODE_CONNECTED;
		node->connections++;

		while (keep_running(node) && handle_request(c, node) == 0) {
			// one request per iteration
		}

		plat_close(c);
	}

	plat_unlisten();
	return NULL;
}
