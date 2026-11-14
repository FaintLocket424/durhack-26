#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  WHBProcInit();
  WHBLogConsoleInit();

  WHBLogPrintf("Hello from Nintendo Wii U!");
  WHBLogPrintf("Press HOME to exit.");

  while (WHBProcIsRunning()) {
    WHBLogConsoleDraw();
    OSSleepTicks(OSMillisecondsToTicks(16));
  }

  WHBLogConsoleFree();
  WHBProcShutdown();
  return 0;
}
