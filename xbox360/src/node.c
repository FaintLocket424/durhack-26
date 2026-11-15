//---------------------------------------------------------------------------------
// node.c - Xbox 360 (libxenon / lwIP raw API) port of wii/src/node.c.
//
// One coordinator at a time, like the Wii node (listen backlog 1). Extra
// connections are refused until the current one closes.
//
// Data flow:
//   on_recv  -> append to rx buffer -> process_frames() -> dispatch_request()
//            -> queue the reply frame in tx -> try_send()
//   on_sent  -> try_send() the rest; once tx is empty, carry on with any
//               frames that are already buffered.
//
// While a reply is still being sent we stop acknowledging received data
// (tcp_recved), so the TCP window closes and the coordinator slows down
// instead of rx growing without limit.
//---------------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/ip_addr.h>
#include <lwip/tcp.h>
#include <network/network.h>

#include "handlers.h"
#include "node.h"
#include "protocol.h"

typedef struct {
  struct tcp_pcb *pcb;

  uint8_t *rx;     // bytes received but not yet parsed
  uint32_t rx_len; // valid bytes in rx
  uint32_t rx_cap;
  uint32_t unacked; // received bytes we haven't tcp_recved() yet

  uint8_t *tx; // reply frame currently being sent (header + payload)
  uint32_t tx_len;
  uint32_t tx_off; // bytes already handed to tcp_write()

  int close_after_send; // stream is broken (bad magic etc.): drop after reply
} conn_t;

static node_t *g_node;
static struct tcp_pcb *g_listener;
static conn_t g_conn;

//---------------------------------------------------------------------------------
static void note_error(int code) {
  //---------------------------------------------------------------------------------
  g_node->errors++;
  g_node->last_error = code;
}

//---------------------------------------------------------------------------------
static void conn_reset(void) {
  //---------------------------------------------------------------------------------
  free(g_conn.rx);
  free(g_conn.tx);
  memset(&g_conn, 0, sizeof g_conn);
  if (g_node->state == NODE_CONNECTED)
    g_node->state = NODE_LISTENING;
}

//---------------------------------------------------------------------------------
static void conn_close(void) {
  //---------------------------------------------------------------------------------
  struct tcp_pcb *pcb = g_conn.pcb;
  if (pcb != NULL) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK)
      tcp_abort(pcb); // out of memory for the FIN: just reset it
  }
  conn_reset();
}

//---------------------------------------------------------------------------------
// tcp_recved() takes a u16_t, so acknowledge large amounts in chunks.
//---------------------------------------------------------------------------------
static void ack_received(uint32_t n) {
  //---------------------------------------------------------------------------------
  while (n > 0) {
    u16_t chunk = n > 0xFFFF ? 0xFFFF : (u16_t)n;
    tcp_recved(g_conn.pcb, chunk);
    n -= chunk;
  }
}

//---------------------------------------------------------------------------------
// Push as much of tx into lwIP as its send buffer allows. lwIP calls on_sent()
// as the coordinator ACKs, and that calls us again for the rest.
// Returns 1 when the whole frame has been queued, 0 if some is still waiting.
//---------------------------------------------------------------------------------
static int try_send(void) {
  //---------------------------------------------------------------------------------
  while (g_conn.tx_off < g_conn.tx_len) {
    uint32_t left = g_conn.tx_len - g_conn.tx_off;
    uint32_t room = tcp_sndbuf(g_conn.pcb);
    uint32_t n = left < room ? left : room;
    if (n > 0xFFFF)
      n = 0xFFFF;
    if (n == 0)
      break; // send buffer full, wait for on_sent()

    err_t err = tcp_write(g_conn.pcb, g_conn.tx + g_conn.tx_off, (u16_t)n,
                          TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM)
      break; // out of segments, wait for on_sent()
    if (err != ERR_OK) {
      note_error(err);
      return 0;
    }
    g_conn.tx_off += n;
  }
  tcp_output(g_conn.pcb);

  if (g_conn.tx_off < g_conn.tx_len)
    return 0;

  free(g_conn.tx);
  g_conn.tx = NULL;
  g_conn.tx_len = g_conn.tx_off = 0;
  return 1;
}

//---------------------------------------------------------------------------------
// Header and payload go out as one buffer (same reason as the Wii: avoids
// Nagle + delayed-ACK stalls between two small writes).
//---------------------------------------------------------------------------------
static int queue_frame(uint16_t type, uint32_t request_id,
                       const uint8_t *payload, uint32_t len) {
  //---------------------------------------------------------------------------------
  frame_header_t h = {
      .type = type, .flags = 0, .request_id = request_id, .payload_len = len};

  uint8_t *frame = malloc(PROTO_HEADER_SIZE + len);
  if (frame == NULL)
    return PROTO_ERR_NOMEM;

  proto_encode_header(frame, &h);
  if (len)
    memcpy(frame + PROTO_HEADER_SIZE, payload, len);

  g_conn.tx = frame;
  g_conn.tx_len = PROTO_HEADER_SIZE + len;
  g_conn.tx_off = 0;
  return PROTO_OK;
}

//---------------------------------------------------------------------------------
static void queue_error(uint32_t request_id, int status) {
  //---------------------------------------------------------------------------------
  uint8_t code[4];
  put_u32(code, (uint32_t)-status);
  queue_frame(MSG_ERROR, request_id, code, sizeof code);
}

//---------------------------------------------------------------------------------
static void consume_rx(uint32_t n) {
  //---------------------------------------------------------------------------------
  memmove(g_conn.rx, g_conn.rx + n, g_conn.rx_len - n);
  g_conn.rx_len -= n;
}

//---------------------------------------------------------------------------------
// Handle every complete frame in rx, one reply at a time.
// Returns -1 if the connection was closed.
//---------------------------------------------------------------------------------
static int process_frames(void) {
  //---------------------------------------------------------------------------------
  while (g_conn.pcb != NULL && g_conn.tx == NULL) {
    if (g_conn.close_after_send) {
      conn_close();
      return -1;
    }

    // Nothing waiting to go out, so let the coordinator send more.
    if (g_conn.unacked) {
      ack_received(g_conn.unacked);
      g_conn.unacked = 0;
    }

    if (g_conn.rx_len < PROTO_HEADER_SIZE)
      return 0;

    frame_header_t h;
    int status = proto_decode_header(g_conn.rx, &h);
    if (status != PROTO_OK) {
      // Bad magic or absurd length: the stream can't be trusted any more.
      note_error(status);
      queue_error(0, status);
      g_conn.close_after_send = 1;
      g_conn.rx_len = 0;
      try_send();
      continue;
    }

    if (g_conn.rx_len < PROTO_HEADER_SIZE + h.payload_len)
      return 0; // wait for the rest of the payload

    response_t resp;
    status = dispatch_request(h.type, g_conn.rx + PROTO_HEADER_SIZE,
                              h.payload_len, &resp);
    consume_rx(PROTO_HEADER_SIZE + h.payload_len);

    if (status == PROTO_OK)
      status = queue_frame(resp.type, h.request_id, resp.data, resp.len);
    free(resp.data);

    if (status != PROTO_OK) {
      // A bad request is not fatal, the connection stays usable.
      note_error(status);
      queue_error(h.request_id, status);
    }

    g_node->requests++;
    try_send();
  }
  return 0;
}

//---------------------------------------------------------------------------------
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                     err_t err) {
  //---------------------------------------------------------------------------------
  (void)arg;
  (void)pcb;

  if (p == NULL) { // coordinator closed the connection
    conn_close();
    return ERR_OK;
  }
  if (err != ERR_OK) {
    pbuf_free(p);
    return err;
  }

  uint32_t need = g_conn.rx_len + p->tot_len;
  if (need > g_conn.rx_cap) {
    uint32_t cap = g_conn.rx_cap ? g_conn.rx_cap : 4096;
    while (cap < need)
      cap *= 2;
    uint8_t *grown = realloc(g_conn.rx, cap);
    if (grown == NULL) {
      // Returning an error makes lwIP keep the pbuf and retry later.
      note_error(PROTO_ERR_NOMEM);
      return ERR_MEM;
    }
    g_conn.rx = grown;
    g_conn.rx_cap = cap;
  }

  pbuf_copy_partial(p, g_conn.rx + g_conn.rx_len, p->tot_len, 0);
  g_conn.rx_len += p->tot_len;
  g_conn.unacked += p->tot_len; // acked in process_frames() once tx is idle
  pbuf_free(p);

  process_frames(); // may tcp_close() the pcb, which is fine to return ERR_OK
                    // after
  return ERR_OK;
}

//---------------------------------------------------------------------------------
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
  //---------------------------------------------------------------------------------
  (void)arg;
  (void)pcb;
  (void)len;

  if (g_conn.tx != NULL && !try_send())
    return ERR_OK;
  process_frames(); // may tcp_close() the pcb, which is fine to return ERR_OK
                    // after
  return ERR_OK;
}

//---------------------------------------------------------------------------------
// The pcb has already been freed by lwIP when this runs (RST, timeout...).
//---------------------------------------------------------------------------------
static void on_err(void *arg, err_t err) {
  //---------------------------------------------------------------------------------
  (void)arg;
  note_error(err);
  g_conn.pcb = NULL;
  conn_reset();
}

//---------------------------------------------------------------------------------
static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
  //---------------------------------------------------------------------------------
  (void)arg;

  if (err != ERR_OK || newpcb == NULL)
    return ERR_VAL;

  tcp_accepted(g_listener);

  if (g_conn.pcb != NULL) {
    // Already serving a coordinator: refuse, like a full listen backlog.
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  memset(&g_conn, 0, sizeof g_conn);
  g_conn.pcb = newpcb;
  g_node->state = NODE_CONNECTED;
  g_node->connections++;

  tcp_setprio(newpcb, TCP_PRIO_NORMAL);
  tcp_nagle_disable(newpcb); // replies are one write each, send them now
  tcp_recv(newpcb, on_recv);
  tcp_sent(newpcb, on_sent);
  tcp_err(newpcb, on_err);
  return ERR_OK;
}

//---------------------------------------------------------------------------------
static int fail(node_t *node, int error) {
  //---------------------------------------------------------------------------------
  node->last_error = error;
  node->state = NODE_FAILED;
  return -1;
}

//---------------------------------------------------------------------------------
int node_start(node_t *node) {
  //---------------------------------------------------------------------------------
  g_node = node;
  node->state = NODE_STARTING;

  // DHCP; on failure libxenon falls back to a static 192.168.1.99.
  if (network_init() == NETWORK_INIT_FAILURE)
    return fail(node, -1);

  strncpy(node->local_ip, ipaddr_ntoa(&netif.ip_addr),
          sizeof node->local_ip - 1);

  struct tcp_pcb *pcb = tcp_new();
  if (pcb == NULL)
    return fail(node, ERR_MEM);

  err_t err = tcp_bind(pcb, IP_ADDR_ANY, node->port);
  if (err != ERR_OK) {
    tcp_close(pcb);
    return fail(node, err);
  }

  g_listener = tcp_listen_with_backlog(pcb, 1); // frees pcb on success
  if (g_listener == NULL) {
    tcp_close(pcb);
    return fail(node, ERR_MEM);
  }
  tcp_accept(g_listener, on_accept);

  node->state = NODE_LISTENING;
  return 0;
}

//---------------------------------------------------------------------------------
void node_poll(node_t *node) {
  //---------------------------------------------------------------------------------
  (void)node;
  network_poll();
}

//---------------------------------------------------------------------------------
void node_stop(node_t *node) {
  //---------------------------------------------------------------------------------
  (void)node;
  conn_close();
  if (g_listener != NULL) {
    tcp_close(g_listener);
    g_listener = NULL;
  }
}
