#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <proc_ui/procui.h>
#include <stdio.h>
#include <string.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

// When the OS requests foreground release (e.g. HOME button or system exit),
// signal WHBProc to stop so the main loop terminates cleanly.
static uint32_t OnProcUIRelease(void *context) {
  (void)context;
  WHBLogPrintf(
      ">>> [WII U] ProcUI release callback triggered. Stopping... <<<");
  WHBProcStopRunning();
  return 0;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  WHBProcInit();
  WHBLogCafeInit();
  WHBLogPrintf(">>> [WII U] App starting! <<<");

  // Intercept OS release (HOME button, shutdown) to exit loop gracefully
  ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, OnProcUIRelease, NULL, 100);

  // Vibrate GamePad to physically confirm startup (~1 sec pulse)
  uint8_t rumble_pattern[15];
  memset(rumble_pattern, 0xFF, sizeof(rumble_pattern));
  VPADControlMotor(VPAD_CHAN_0, rumble_pattern, 120);

  // Initialize display
  OSScreenInit();
  size_t tvBufferSize = OSScreenGetBufferSizeEx(SCREEN_TV);
  size_t drcBufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

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
    WHBLogPrintf(">>> [WII U] Failed to allocate screen buffers! <<<");
    if (tvBuffer)
      free(tvBuffer);
    if (drcBuffer)
      free(drcBuffer);
    OSScreenShutdown();
    WHBLogCafeDeinit();
    WHBProcShutdown();
    return 1;
  }

  OSScreenSetBufferEx(SCREEN_TV, tvBuffer);
  OSScreenSetBufferEx(SCREEN_DRC, drcBuffer);
  OSScreenEnableEx(SCREEN_TV, TRUE);
  OSScreenEnableEx(SCREEN_DRC, TRUE);

  WHBLogPrintf(">>> [WII U] Entering render loop <<<");

  int frame = 0;
  while (WHBProcIsRunning()) {
    VPADStatus vpad;
    VPADReadError vpadErr;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadErr);

    // Press (A) or (B) to exit directly as well
    if (vpad.trigger & (VPAD_BUTTON_A | VPAD_BUTTON_B)) {
      WHBLogPrintf(">>> [WII U] User pressed A/B, exiting! <<<");
      break;
    }

    // RGBA colors (0xRRGGBBAA)
    OSScreenClearBufferEx(SCREEN_TV, 0x103080FF);
    OSScreenClearBufferEx(SCREEN_DRC, 0x006030FF);

    char buf[64];
    snprintf(buf, sizeof(buf), "Active Frame: %d", frame++);

    // GamePad text
    OSScreenPutFontEx(SCREEN_DRC, 0, 0, "DurHack Wii U - Running on Hardware!");
    OSScreenPutFontEx(SCREEN_DRC, 0, 2,
                      "Press HOME or (A) on GamePad to exit.");
    OSScreenPutFontEx(SCREEN_DRC, 0, 4, buf);

    // TV text
    OSScreenPutFontEx(SCREEN_TV, 0, 0, "DurHack Wii U - TV Display Active!");
    OSScreenPutFontEx(SCREEN_TV, 0, 2, "Mirroring GamePad output.");
    OSScreenPutFontEx(SCREEN_TV, 0, 4, buf);

    // Flush CPU data caches and present
    DCFlushRange(tvBuffer, tvBufferSize);
    DCFlushRange(drcBuffer, drcBufferSize);

    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);

    OSSleepTicks(OSMillisecondsToTicks(16));
  }

  WHBLogPrintf(">>> [WII U] Exiting cleanly and freeing resources... <<<");

  // Disable display scanout before freeing framebuffers
  OSScreenEnableEx(SCREEN_TV, FALSE);
  OSScreenEnableEx(SCREEN_DRC, FALSE);

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
