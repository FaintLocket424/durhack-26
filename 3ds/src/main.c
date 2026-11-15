#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <3ds.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

// Configuration for Head Node
#define SERVER_IP       "10.19.99.172" 
#define SERVER_PORT     8081        

static u32 *SOC_buffer = NULL;
s32 sock = -1;

__attribute__((format(printf,1,2)))
void failExit(const char *fmt, ...);

void socShutdown() {
    printf("waiting for socExit...\n");
    socExit();
}

int main(int argc, char **argv) {
    int ret;
    struct sockaddr_in server;

    gfxInitDefault();
    atexit(gfxExit);
    consoleInit(GFX_TOP, NULL);

    printf("\n3DS Parallel Ray Tracer Client\n");

    // Allocate buffer for SOC service
    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(SOC_buffer == NULL) {
        failExit("memalign: failed to allocate\n");
    }

    // Initialize soc:u service
    if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0) {
        failExit("socInit: 0x%08X\n", (unsigned int)ret);
    }
    atexit(socShutdown);

    // Create TCP socket
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        failExit("socket: %d %s\n", errno, strerror(errno));
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server.sin_addr);

    printf("Connecting to server at %s:%d...\n", SERVER_IP, SERVER_PORT);

    // Connect to the head node
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        while (aptMainLoop()) {
            gspWaitForVBlank();
            hidScanInput();
            if (hidKeysDown() & KEY_B) break;
        }
        close(sock);
        failExit("connect: %d %s\n", errno, strerror(errno));
    }

    printf("Connected successfully! Ready for ray tracing tasks.\n");

    // Main client loop
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();

        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        char *test_msg = "PING\n";
        send(sock, test_msg, strlen(test_msg), 0);

        char rx_buffer[32];
        memset(rx_buffer, 0, sizeof(rx_buffer));
        int bytes_received = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (bytes_received > 0) {
            printf("Server response: %s", rx_buffer);
        }
    }

    close(sock);
    return 0;
}

void failExit(const char *fmt, ...) {
    if(sock > 0) close(sock);

    va_list ap;
    printf(CONSOLE_RED);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(CONSOLE_RESET);
    printf("\nPress B to exit\n");

    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_B) exit(0);
    }
}