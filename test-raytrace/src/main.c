#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

static void *xfb[2];
static GXRModeObj *rmode;

// BT.601 RGB -> YCbCr, packed for two pixels (chroma averaged)
static inline u32 rgb2yuv_pair(u8 r0,u8 g0,u8 b0, u8 r1,u8 g1,u8 b1) {
    int y0 = ((66*r0 + 129*g0 +  25*b0 + 128) >> 8) + 16;
    int y1 = ((66*r1 + 129*g1 +  25*b1 + 128) >> 8) + 16;
    int r = (r0+r1)>>1, g = (g0+g1)>>1, b = (b0+b1)>>1;
    int cb = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
    int cr = ((112*r -  94*g -  18*b + 128) >> 8) + 128;
    return (y0<<24) | (cb<<16) | (y1<<8) | cr;
}

int main(void) {
    VIDEO_Init();
    WPAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);

    xfb[0] = SYS_AllocateFramebuffer(rmode);   // cached (K0) pointers
    xfb[1] = SYS_AllocateFramebuffer(rmode);

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    const int W = rmode->fbWidth;      // 640
    const int H = rmode->xfbHeight;    // ~480
    const u32 fbsize = VIDEO_GetFrameBufferSize(rmode);
    int fb = 0;

    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);

        u32 *out = (u32*)xfb[fb];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x += 2) {
                // TODO: trace two pixels here
                u8 r0 = x*255/W, g0 = y*255/H, b0 = 128;
                u8 r1 = (x+1)*255/W, g1 = g0, b1 = 128;
                *out++ = rgb2yuv_pair(r0,g0,b0, r1,g1,b1);
            }
        }

        DCFlushRange(xfb[fb], fbsize);         // push cache to RAM before scanout
        VIDEO_SetNextFramebuffer(xfb[fb]);
        VIDEO_Flush();
        VIDEO_WaitVSync();
        fb ^= 1;
    }
    return 0;
}
