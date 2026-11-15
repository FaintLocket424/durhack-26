// Copy core/{node,handlers,render}.{c,h}, protocol.h and platform.h into this folder.
// Arduino builds the .c files in the sketch folder automatically.
extern "C" {
#include "node.h"
}

static node_t node;

void setup() {
	Serial.begin(115200);
	node.port = 5555;
}

void loop() {
	node_run(&node);								// blocks; returns only if init failed
	Serial.printf("node stopped, err %d\n", node.last_error);
	delay(5000);
	node.stop = 0;									// retry
}
