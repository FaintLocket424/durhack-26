#include "tracer.h"

#include <gccore.h>
#include <wiiuse/wpad.h>

int main(void) {
    VIDEO_Init();
    WPAD_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    u32 *xfb = (u32 *)SYS_AllocateFramebuffer(rmode);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    init_scene();
    const int W = rmode->fbWidth;
    const int H = rmode->xfbHeight;

    light_dir = norm(v3(-0.6f, 1.0f, 0.4f));

    u32 black = pack_yuv(v3(0, 0, 0));
    for (int i = 0; i < RW * RH; i++) renderbuf[i] = black;
    present(xfb, W, H);

    for (int ty = 0; ty < RH; ty += TILE) {
        for (int tx = 0; tx < RW; tx += TILE)
        {
            render_tile(tx, ty);

            present(xfb, W, H);
            VIDEO_WaitVSync(); // visible tiling
        }
        
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
    }

    // and now we wait to end
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
        VIDEO_WaitVSync();
    }
    return 0;
}