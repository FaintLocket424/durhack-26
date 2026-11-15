#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // 1. Initialize process lifecycle and CafeOS syslog
  WHBProcInit();
  WHBLogCafeInit();
  WHBLogPrintf(">>> [WII U] main() entered successfully! <<<");

  // 2. GamePad vibration confirmation (~1 second pulse)
  uint8_t rumble_pattern[15];
  memset(rumble_pattern, 0xFF, sizeof(rumble_pattern));
  VPADControlMotor(VPAD_CHAN_0, rumble_pattern, 120);

  // 3. Initialize OSScreen
  OSScreenInit();

  size_t tvBufferSize = OSScreenGetBufferSizeEx(SCREEN_TV);
  size_t drcBufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

  // Allocate 0x100-aligned framebuffers from default heap (or memalign)
  void *tvBuffer = NULL;
  void *drcBuffer = NULL;

  if (MEMAllocFromDefaultHeapEx) {
    tvBuffer = MEMAllocFromDefaultHeapEx(tvBufferSize, 0x100);
    drcBuffer = MEMAllocFromDefaultHeapEx(drcBufferSize, 0x100);
  } else {
    tvBuffer = memalign(0x100, tvBufferSize);
    drcBuffer = memalign(0x100, drcBufferSize);
  }

  if (!tvBuffer || !drcBuffer) {
    WHBLogPrintf(">>> [WII U] Framebuffer alloc failed! tv=%p drc=%p <<<",
                 tvBuffer, drcBuffer);
    if (tvBuffer) {
      if (MEMFreeToDefaultHeap)
        MEMFreeToDefaultHeap(tvBuffer);
      else
        free(tvBuffer);
    }
    if (drcBuffer) {
      if (MEMFreeToDefaultHeap)
        MEMFreeToDefaultHeap(drcBuffer);
      else
        free(drcBuffer);
    }
    OSScreenShutdown();
    WHBLogCafeDeinit();
    WHBProcShutdown();
    return 1;
  }

  OSScreenSetBufferEx(SCREEN_TV, tvBuffer);
  OSScreenSetBufferEx(SCREEN_DRC, drcBuffer);
  OSScreenEnableEx(SCREEN_TV, TRUE);
  OSScreenEnableEx(SCREEN_DRC, TRUE);

  WHBLogPrintf(">>> [WII U] OSScreen initialized! Entering loop. <<<");

  int frame = 0;
  while (WHBProcIsRunning()) {
    VPADStatus vpad;
    VPADReadError vpadErr;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadErr);

    // Clear screen buffers (RGBA8 format): Dark blue for TV, dark green for DRC
    OSScreenClearBufferEx(SCREEN_TV, 0x00104000);
    OSScreenClearBufferEx(SCREEN_DRC, 0x00401000);

    // Print text
    OSScreenPutFontEx(SCREEN_DRC, 0, 0,
                      "DurHack Wii U - Code Running on Hardware!");
    OSScreenPutFontEx(SCREEN_DRC, 0, 2,
                      "Press HOME button on GamePad to exit.");

    char buf[64];
    snprintf(buf, sizeof(buf), "Active Frame: %d", frame++);
    OSScreenPutFontEx(SCREEN_DRC, 0, 4, buf);

    OSScreenPutFontEx(SCREEN_TV, 0, 0, "DurHack Wii U - TV Display Active!");
    OSScreenPutFontEx(SCREEN_TV, 0, 2, "GamePad is mirroring info.");
    OSScreenPutFontEx(SCREEN_TV, 0, 4, buf);

    // Flush CPU data cache before flipping
    DCFlushRange(tvBuffer, tvBufferSize);
    DCFlushRange(drcBuffer, drcBufferSize);

    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);

    OSSleepTicks(OSMillisecondsToTicks(16));
  }

  // 4. Cleanup on exit
  WHBLogPrintf(">>> [WII U] Exiting cleanly... <<<");
  if (MEMFreeToDefaultHeap) {
    MEMFreeToDefaultHeap(tvBuffer);
    MEMFreeToDefaultHeap(drcBuffer);
  } else {
    free(tvBuffer);
    free(drcBuffer);
  }

  OSScreenShutdown();
  WHBLogCafeDeinit();
  WHBProcShutdown();
  return 0;
}
