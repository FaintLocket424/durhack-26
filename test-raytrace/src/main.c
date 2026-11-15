// Recursive CPU raytracer for Wii (libogc). Writes straight to the XFB.
// Adds: reflection, refraction (Snell + Schlick Fresnel), tinted shadows, supersampling.
#include <stdlib.h>
#include <math.h>
#include <gccore.h>
#include <wiiuse/wpad.h>

#define RW        320   // render width  (1 render pixel = 1 XFB word = 2 XFB pixels at 640 wide)
#define RH        240
#define TILE      16
#define EPS       1e-3f
#define PLANE_Y   (-1.0f)

#define MAX_DEPTH 4     // recursion limit. Glass branches in two, so cost grows fast
#define SAMPLES   2     // SAMPLES x SAMPLES rays per render pixel (1 = off)
#define AMBIENT   0.15f

// ---------------------------------------------------------------- vec3
typedef struct { float x, y, z; } vec3;

static inline vec3  v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3  add(vec3 a, vec3 b)    { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3  sub(vec3 a, vec3 b)    { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3  mul(vec3 a, float s)   { return v3(a.x*s, a.y*s, a.z*s); }
static inline float dot(vec3 a, vec3 b)    { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3  norm(vec3 a)           { return mul(a, 1.0f / sqrtf(dot(a, a))); }

static inline vec3 reflect_dir(vec3 d, vec3 n) { return sub(d, mul(n, 2.0f * dot(d, n))); }

// Snell's law. n must face the incoming ray. Returns 0 on total internal reflection.
static inline int refract_dir(vec3 d, vec3 n, float eta, vec3 *out) {
    float cosi = -dot(n, d);
    float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
    if (k < 0.0f) return 0;
    *out = add(mul(d, eta), mul(n, eta * cosi - sqrtf(k)));
    return 1;
}

// ---------------------------------------------------------------- scene
typedef struct {
    vec3  col;      // surface colour
    float refl;     // 0..1 mirror weight for opaque materials
    float transp;   // 0..1 how much of the colour comes from reflect+refract (1 = pure glass)
    float ior;      // index of refraction
} material;

typedef struct { vec3 c; float r; material m; } sphere;

static const sphere spheres[] = {
    // glass ball
    {{ 0.0f,  0.0f, -5.0f}, 1.0f, {{0.90f, 0.95f, 1.00f}, 0.0f, 0.90f, 1.5f}},
    // mirror ball
    {{ 2.2f, -0.4f, -6.0f}, 0.6f, {{0.20f, 0.80f, 0.30f}, 0.6f, 0.0f, 1.0f}},
    // glossy blue ball
    {{-2.0f, -0.3f, -4.5f}, 0.7f, {{0.25f, 0.35f, 0.90f}, 0.25f, 0.0f, 1.0f}},
    // red ball behind the glass (so you can see refraction)
    {{ 0.3f, -0.4f, -8.5f}, 0.6f, {{0.90f, 0.20f, 0.20f}, 0.1f, 0.0f, 1.0f}},
};
#define NSPHERES ((int)(sizeof(spheres)/sizeof(spheres[0])))

static vec3 light_dir;   // direction TO the light, normalised

typedef struct { float t; vec3 p, n; material m; } hit;

static float hit_sphere(const sphere *s, vec3 o, vec3 d) {
    vec3 oc = sub(o, s->c);
    float b = dot(oc, d);
    float c = dot(oc, oc) - s->r * s->r;
    float disc = b*b - c;
    if (disc < 0.0f) return -1.0f;
    float sq = sqrtf(disc);
    float t = -b - sq;
    if (t > EPS) return t;
    t = -b + sq;                          // origin inside the sphere
    return (t > EPS) ? t : -1.0f;
}

static int scene_hit(vec3 o, vec3 d, hit *h) {
    float best = 1e30f;
    int which = -1;                       // -1 none, 0..N-1 sphere, NSPHERES = plane

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
        h->m.col    = chk ? v3(0.85f, 0.85f, 0.85f) : v3(0.25f, 0.25f, 0.25f);
        h->m.refl   = 0.25f;
        h->m.transp = 0.0f;
        h->m.ior    = 1.0f;
    } else {
        h->n = mul(sub(h->p, spheres[which].c), 1.0f / spheres[which].r);
        h->m = spheres[which].m;
    }
    return 1;
}

// Light transmittance toward the light: 1 = fully lit, 0 = blocked.
// Glass lets some light through, so it casts a lighter shadow.
static float shadow_factor(vec3 o, vec3 d) {
    float f = 1.0f;
    for (int i = 0; i < NSPHERES; i++) {
        if (hit_sphere(&spheres[i], o, d) > 0.0f) {
            f *= spheres[i].m.transp;
            if (f < 0.01f) return 0.0f;
        }
    }
    return f;
}

static vec3 sky(vec3 d) {
    float t = 0.5f * (d.y + 1.0f);
    return add(mul(v3(1.0f, 1.0f, 1.0f), 1.0f - t), mul(v3(0.5f, 0.7f, 1.0f), t));
}

// ---------------------------------------------------------------- recursive trace
static vec3 trace(vec3 o, vec3 d, int depth) {
    if (depth <= 0) return sky(d);        // cut-off: fall back to sky, not black

    hit h;
    if (!scene_hit(o, d, &h)) return sky(d);

    vec3 N = h.n;
    int entering = dot(N, d) < 0.0f;
    if (!entering) N = mul(N, -1.0f);     // make N face the incoming ray

    // ---- local lighting: ambient + diffuse + Blinn-Phong highlight
    float diff = dot(N, light_dir);
    float spec = 0.0f;
    if (diff < 0.0f) diff = 0.0f;
    if (diff > 0.0f) {
        diff *= shadow_factor(add(h.p, mul(N, EPS)), light_dir);
        if (diff > 0.0f) {
            float s = dot(N, norm(sub(light_dir, d)));
            if (s > 0.0f) {               // s^32 by repeated squaring (pow is slow)
                s *= s; s *= s; s *= s; s *= s; s *= s;
                spec = 0.6f * s * diff;
            }
        }
    }
    vec3 local = add(mul(h.m.col, AMBIENT + (1.0f - AMBIENT) * diff), v3(spec, spec, spec));

    if (h.m.refl <= 0.0f && h.m.transp <= 0.0f) return local;

    vec3 rdir = reflect_dir(d, N);
    vec3 mirror_o = add(h.p, mul(N, EPS));

    // ---- opaque, partly mirrored
    if (h.m.transp <= 0.0f) {
        vec3 rc = trace(mirror_o, rdir, depth - 1);
        return add(mul(local, 1.0f - h.m.refl), mul(rc, h.m.refl));
    }

    // ---- transparent: Fresnel split between reflection and refraction
    float n1 = entering ? 1.0f : h.m.ior;
    float n2 = entering ? h.m.ior : 1.0f;
    float eta = n1 / n2;
    float cosi = -dot(N, d);

    vec3 tdir;
    int can_refract = refract_dir(d, N, eta, &tdir);

    float R;
    if (!can_refract) {
        R = 1.0f;                         // total internal reflection
    } else {
        float r0 = (n1 - n2) / (n1 + n2); r0 *= r0;
        float c = (n1 <= n2) ? cosi : -dot(N, tdir);   // Schlick uses the larger-angle side
        float m = 1.0f - c;
        float m2 = m * m;
        R = r0 + (1.0f - r0) * m2 * m2 * m;
    }
    float T = 1.0f - R;

    vec3 fres = mul(trace(mirror_o, rdir, depth - 1), R);
    if (can_refract) {
        vec3 refr_o = sub(h.p, mul(N, EPS));          // start on the other side
        fres = add(fres, mul(trace(refr_o, tdir, depth - 1), T));
    }

    float k = h.m.transp;
    // tint what passes through by the material colour so glass isn't perfectly clear
    fres = v3(fres.x * h.m.col.x, fres.y * h.m.col.y, fres.z * h.m.col.z);
    return add(mul(local, 1.0f - k), mul(fres, k));
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
    const float inv = 1.0f / (SAMPLES * SAMPLES);

    for (int y = y0; y < y0 + TILE; y++) {
        for (int x = x0; x < x0 + TILE; x++) {
            vec3 sum = {0.0f, 0.0f, 0.0f};
            for (int sy = 0; sy < SAMPLES; sy++) {
                for (int sx = 0; sx < SAMPLES; sx++) {
                    float fx = x + (sx + 0.5f) / SAMPLES;
                    float fy = y + (sy + 0.5f) / SAMPLES;
                    vec3 d = norm(v3((fx - RW * 0.5f) * px,
                                     (RH * 0.5f - fy) * px,
                                     -1.0f));
                    sum = add(sum, trace(origin, d, MAX_DEPTH));
                }
            }
            renderbuf[y * RW + x] = pack_yuv(mul(sum, inv));
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

    for (int ty = 0; ty < RH; ty += TILE) {
        for (int tx = 0; tx < RW; tx += TILE)
            render_tile(tx, ty);

        present(xfb, W, H);
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
    }

    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
        VIDEO_WaitVSync();
    }
    return 0;
}