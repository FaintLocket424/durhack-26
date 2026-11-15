// Arduino-API targets with WiFi: ESP32 (recommended), ESP8266, Pico W (arduino-pico).
// NOT classic AVR Uno/Mega: ~2-8 KB RAM, no FPU, no TCP stack. Not feasible.
// Single thread: node_run() is called from loop(); plat_idle()/plat_preview() yield.
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

extern "C" {
#include "platform.h"
}

static WiFiServer *server = nullptr;
static WiFiClient client;

extern "C" {

int plat_net_init(char *ip, size_t len) {
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	for (int i = 0; WiFi.status() != WL_CONNECTED; i++) {
		if (i > 60) return -1;							// ~30 s
		delay(500);
	}
	strncpy(ip, WiFi.localIP().toString().c_str(), len - 1);
	return 0;
}

bool plat_keep_running(void)    { return true; }
void plat_idle(void)            { delay(1); }			// feeds the watchdog / WiFi task
void plat_sleep_ms(unsigned ms) { delay(ms); }

int plat_listen(uint16_t port) {
	server = new WiFiServer(port);
	server->begin();
	server->setNoDelay(true);							// same reason as the Wii: no Nagle stalls
	return 0;
}
void plat_unlisten(void) { if (server) { server->end(); delete server; server = nullptr; } }

int plat_accept(void) {
	WiFiClient c = server->available();					// core 3.x: accept() also works
	if (!c) return PLAT_AGAIN;
	client = c;
	return 0;											// single connection: handle 0
}

int plat_recv(plat_conn_t, void *buf, size_t len) {
	int avail = client.available();
	if (avail <= 0) return client.connected() ? PLAT_AGAIN : -1;
	int n = client.read((uint8_t *)buf, len < (size_t)avail ? len : (size_t)avail);
	return n > 0 ? n : -1;
}

int plat_send(plat_conn_t, const void *buf, size_t len) {
	size_t n = client.write((const uint8_t *)buf, len);
	if (n == 0) return client.connected() ? PLAT_AGAIN : -1;
	return (int)n;
}

void plat_close(plat_conn_t) { client.stop(); }

int plat_preview(const tile_job_t *, const float *, uint32_t) {
	yield();											// no display; just keep the system alive
	return 0;
}

}  // extern "C"
