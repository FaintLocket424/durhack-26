#include <3ds.h>
#include <stdio.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  gfxInitDefault();
  consoleInit(GFX_TOP, NULL);

  printf("\x1b[16;13HHello from Nintendo 3DS!\n");
  printf("\x1b[30;15HPress START to exit.\n");

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    if (kDown & KEY_START)
      break;

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }

  gfxExit();
  return 0;
}
