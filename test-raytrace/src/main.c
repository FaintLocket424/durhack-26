// Minimal CPU raytracer for Wii (libogc). Writes straight to the XFB (external framebuffer)
#include <stdlib.h>
#include <math.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#define RW    320      // render width  (each render pixel = 2 XFB pixels wide at 640)
#define RH    240      // render height
#define TILE  16       // tile size in render pixels
#define EPS   1e-3f
#define PLANE_Y (-1.0f)

// vec3
typedef struct { float x, y, z; } vec3;

static inline vec3  v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3  add(vec3 a, vec3 b)    { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3  sub(vec3 a, vec3 b)    { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3  mul(vec3 a, float s)   { return v3(a.x*s, a.y*s, a.z*s); }
static inline float dot(vec3 a, vec3 b)    { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3  norm(vec3 a)           { return mul(a, 1.0f / sqrtf(dot(a, a))); }

// scene
typedef struct { vec3 c; float r; vec3 col; } sphere;

static const sphere spheres[] = {
    {{ 0.0f,  0.0f, -5.0f}, 1.0f, {0.90f, 0.20f, 0.20f}},
    {{ 2.2f, -0.4f, -6.0f}, 0.6f, {0.20f, 0.80f, 0.30f}},
    {{-2.0f, -0.3f, -4.5f}, 0.7f, {0.25f, 0.35f, 0.90f}},
};
#define NSPHERES ((int)(sizeof(spheres)/sizeof(spheres[0])))

static vec3 light_dir;   // direction TO the light, normalised

typedef struct { float t; vec3 p, n, col; } hit;

static float hit_sphere(const sphere *s, vec3 o, vec3 d) {
    vec3 oc = sub(o, s->c);
    float b = dot(oc, d);
    float c = dot(oc, oc) - s->r * s->r;
    float disc = b*b - c;
    if (disc < 0.0f) return -1.0f;
    float sq = sqrtf(disc);
    float t = -b - sq;
    if (t > EPS) return t;
    t = -b + sq;
    return (t > EPS) ? t : -1.0f;
}

static int scene_hit(vec3 o, vec3 d, hit *h) {
    float best = 1e30f;
    int which = -1;          // -1 none, 0..N-1 sphere, NSPHERES = plane

    for (int i = 0; i < NSPHERES; i++) {
        float t = hit_sphere(&spheres[i], o, d);
        if (t > 0.0f && t < best) { best = t; which = i; }
    }
    if (d.y < -1e-4f) {
        float t = (PLANE_Y - o.y) / d.y;
        if (t > EPS && t < best) { best = t; which = NSPHERES; }
    }
    if (which < 0) return 0;

    h->t = best;
    h->p = add(o, mul(d, best));
    if (which == NSPHERES) {
        h->n = v3(0, 1, 0);
        int chk = ((int)floorf(h->p.x) + (int)floorf(h->p.z)) & 1;
        h->col = chk ? v3(0.85f, 0.85f, 0.85f) : v3(0.25f, 0.25f, 0.25f);
    } else {
        h->n = mul(sub(h->p, spheres[which].c), 1.0f / spheres[which].r);
        h->col = spheres[which].col;
    }
    return 1;
}

// Shadow test: spheres only (the plane is never between a point and the light)
static int occluded(vec3 o, vec3 d) {
    for (int i = 0; i < NSPHERES; i++)
        if (hit_sphere(&spheres[i], o, d) > 0.0f) return 1;
    return 0;
}

static vec3 trace(vec3 o, vec3 d) {
    hit h;
    if (!scene_hit(o, d, &h)) {
        float t = 0.5f * (d.y + 1.0f);                       // sky gradient
        return add(mul(v3(1.0f, 1.0f, 1.0f), 1.0f - t), mul(v3(0.5f, 0.7f, 1.0f), t));
    }
    float diff = dot(h.n, light_dir);
    if (diff < 0.0f) diff = 0.0f;
    if (diff > 0.0f && occluded(add(h.p, mul(h.n, EPS)), light_dir)) diff = 0.0f;
    return mul(h.col, 0.15f + 0.85f * diff);
}

// ---------------------------------------------------------------- pixel packing
// One render pixel -> one XFB word (two identical pixels: Y Cb Y Cr)
static inline u32 pack_yuv(vec3 c) {
    int r = (int)(c.x * 255.0f), g = (int)(c.y * 255.0f), b = (int)(c.z * 255.0f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    int y  = ((66*r + 129*g +  25*b + 128) >> 8) + 16;
    int cb = ((-38*r -  74*g + 112*b + 128) >> 8) + 128;
    int cr = ((112*r -  94*g -  18*b + 128) >> 8) + 128;
    return ((u32)y << 24) | ((u32)cb << 16) | ((u32)y << 8) | (u32)cr;
}

// ---------------------------------------------------------------- rendering
static u32 renderbuf[RW * RH];

static void render_tile(int x0, int y0) {
    const float px = 2.0f * 0.57735f / RH;    // tan(30deg): 60deg vertical FOV
    const vec3 origin = {0.0f, 0.0f, 0.0f};

    for (int y = y0; y < y0 + TILE; y++) {
        for (int x = x0; x < x0 + TILE; x++) {
            vec3 d = norm(v3((x + 0.5f - RW * 0.5f) * px,
                             (RH * 0.5f - (y + 0.5f)) * px,
                             -1.0f));
            renderbuf[y * RW + x] = pack_yuv(trace(origin, d));
        }
    }
}

// Nearest-neighbour upscale of renderbuf into the XFB
static void present(u32 *xfb, int W, int H) {
    int words = W / 2;                         // u32s per XFB row
    for (int y = 0; y < H; y++) {
        const u32 *src = renderbuf + (y * RH / H) * RW;
        u32 *dst = xfb + y * words;
        for (int i = 0; i < words; i++)
            dst[i] = src[i * RW / words];
    }
    DCFlushRange(xfb, (u32)W * H * 2);         // push cache to RAM before scanout
}

// ---------------------------------------------------------------- main
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

    const int W = rmode->fbWidth;
    const int H = rmode->xfbHeight;

    light_dir = norm(v3(-0.6f, 1.0f, 0.4f));

    u32 black = pack_yuv(v3(0, 0, 0));
    for (int i = 0; i < RW * RH; i++) renderbuf[i] = black;
    present(xfb, W, H);

    // Render one row of tiles, show progress, repeat
    for (int ty = 0; ty < RH; ty += TILE) {
        for (int tx = 0; tx < RW; tx += TILE)
            render_tile(tx, ty);

        present(xfb, W, H);
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
    }

    // Done: keep displaying the image
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
        VIDEO_WaitVSync();
    }
    return 0;
}