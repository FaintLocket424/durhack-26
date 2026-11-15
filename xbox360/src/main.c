// Xbox 360 compute node: boots via XeLL, serves the WRT1 protocol on TCP 5555.
// Single-threaded: the loop below drives networking (node_poll) and the
// status screen. Same behaviour as wii/src/main.c.

#include <stdio.h>

#include <console/console.h>
#include <input/input.h>
#include <ppc/timebase.h>
#include <usb/usbmain.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>

#include "node.h"

#define NODE_PORT 5555

static node_t node = {.port = NODE_PORT};

//---------------------------------------------------------------------------------
static const char *state_name(node_state_t state) {
  //---------------------------------------------------------------------------------
  switch (state) {
  case NODE_STARTING:
    return "configuring network";
  case NODE_LISTENING:
    return "listening          ";
  case NODE_CONNECTED:
    return "client connected   ";
  default:
    return "FAILED             ";
  }
}

//---------------------------------------------------------------------------------
static void draw_status(int top) {
  //---------------------------------------------------------------------------------
  console_set_cursor(0, top);
  printf("state:        %s\n", state_name(node.state));
  printf("address:      %s:%d      \n", node.local_ip, node.port);
  printf("connections:  %u      \n", (unsigned)node.connections);
  printf("requests:     %u      \n", (unsigned)node.requests);
  printf("errors:       %u (last: %d)      \n", (unsigned)node.errors,
         node.last_error);
  printf("\nBACK to quit\n");
}

//---------------------------------------------------------------------------------
int main(void) {
  //---------------------------------------------------------------------------------
  xenon_make_it_faster(XENON_SPEED_FULL);
  xenos_init(VIDEO_MODE_AUTO);
  console_init();
  usb_init();
  usb_do_poll();

  printf("Xbox 360 compute node\n\n");
  node_start(&node); // prints libxenon's DHCP progress

  console_clrscr();
  printf("Xbox 360 compute node\n\n");
  int status_top = console_get_cursor_y();

  struct controller_data_s pad;
  uint64_t last_draw = 0;

  for (;;) {
    node_poll(&node); // all TCP work happens in here

    // Redrawing the console is slow, so only refresh ~10 times a second.
    uint64_t now = mftb();
    if (tb_diff_msec(now, last_draw) >= 100) {
      last_draw = now;
      usb_do_poll();
      draw_status(status_top);
      if (get_controller_data(&pad, 0) && pad.back)
        break;
    }
  }

  node_stop(&node);
  return 0;
}
