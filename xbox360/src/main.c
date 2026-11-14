// Xbox 360 node: boots via XeLL, brings up the network, shows status.

#include <stdio.h>

#include <console/console.h>
#include <input/input.h>
#include <network/network.h>
#include <usb/usbmain.h>
#include <xenon_soc/xenon_power.h>
#include <xenos/xenos.h>

int main(void) {
  xenon_make_it_faster(XENON_SPEED_FULL);
  xenos_init(VIDEO_MODE_AUTO);
  console_init();
  usb_init();
  usb_do_poll();

  printf("Xbox 360 compute node\n\n");
  printf("configuring network (DHCP)...\n");

  int rc = network_init();
  if (rc == NETWORK_INIT_FAILURE) {
    printf("network init FAILED\n");
  } else {
    if (rc == NETWORK_INIT_DHCP_FAILURE)
      printf("DHCP failed, using fallback address\n");
    network_print_config();
  }

  printf("\npress BACK to quit\n");

  struct controller_data_s pad;
  for (;;) {
    network_poll();
    usb_do_poll();
    if (get_controller_data(&pad, 0) && pad.back)
      break;
  }

  return 0;
}
