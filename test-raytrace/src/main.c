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

#define MAX_DEPTH 6     // recursion limit
#define SAMPLES   2     // SAMPLES x SAMPLES rays per render pixel (1 = off)
#define AMBIENT   0.15f

#define SPP_PER_JOB 1        // samples per tile per job
#define MAX_PASSES  0        // 0 = keep refining forever

// ---------------------------------------------------------------- vec3
typedef struct { float x, y, z; } vec3;

static inline vec3  v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3  add(vec3 a, vec3 b)    { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3  sub(vec3 a, vec3 b)    { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3  mul(vec3 a, float s)   { return v3(a.x*s, a.y*s, a.z*s); }
static inline float dot(vec3 a, vec3 b)    { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3  norm(vec3 a)           { return mul(a, 1.0f / sqrtf(dot(a, a))); }
static inline vec3  cross(vec3 a, vec3 b)  { return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); }
static inline vec3 vmul(vec3 a, vec3 b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }

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
    {{ 0.0f,  0.0f, -5.0f}, 1.0f, {{0.90f, 0.95f, 1.00f}, 0.0f, 0.90f, 1.52f}},
    // mirror ball
    {{ 2.2f, -0.4f, -6.0f}, 0.6f, {{0.20f, 0.80f, 0.30f}, 0.9f, 0.0f, 1.0f}},
    // glossy blue ball
    {{-2.0f, -0.3f, -4.5f}, 0.7f, {{0.25f, 0.35f, 0.90f}, 0.25f, 0.35f, 1.5f}},
    // red ball behind the glass (so you can see refraction)
    {{ 0.3f, 0.0f, -8.5f}, 1.f, {{0.90f, 0.50f, 0.20f}, 0.5f, 0.0f, 1.0f}},
};
#define NSPHERES ((int)(sizeof(spheres)/sizeof(spheres[0])))

// A plane through point p with normal n. If hu > 0 it is a finite rectangle:
// u is the in-plane "width" axis, v = cross(n,u) the "height" axis,
// and hu/hv are the half-extents along them. hu <= 0 means infinite.
// If tile > 0 the surface alternates m.col / col2 in squares of that size.
typedef struct {
    vec3  p, n, u, v;
    float hu, hv;
    float tile;
    vec3  col2;
    material m;
} plane;
 
static plane planes[] = {
    // floor (infinite)
    { {0,-1,0}, {0,1,0}, {1,0,0}, {0,0,0}, 0.0f, 0.0f,
      1.0f, {0.125f,0.125f,0.125f}, {{0.85f,0.85f,0.85f}, 0.25f, 0.0f, 1.0f} },
 
    // diagonal wall: 10 wide, 4 tall, turned 45 degrees, front faces the camera side
    { {3.5f, 1.0f, -9.0f}, {-1,0,1}, {1,0,1}, {0,0,0}, 5.0f, 2.0f,
      0.75f, {0.05f,0.05f,0.05f}, {{0.05f,0.05f,0.05f}, 0.35f, 0.0f, 1.0f} },
};
#define NPLANES ((int)(sizeof(planes)/sizeof(planes[0])))

static void init_scene(void) {
    for (int i = 0; i < NPLANES; i++) {
        plane *pl = &planes[i];
        pl->n = norm(pl->n);
        pl->u = norm(sub(pl->u, mul(pl->n, dot(pl->u, pl->n))));   // u perp to n
        pl->v = cross(pl->n, pl->u);
    }
}

static u32 rng_state = 1;
static inline u32 rng_u32(void) {            // xorshift32
    u32 x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}
static inline float rnd(void) { return (rng_u32() >> 8) * (1.0f / 16777216.0f); }

static vec3 cos_hemisphere(vec3 n) {
    float r1 = 6.2831853f * rnd(), r2 = rnd(), s = sqrtf(r2);
    vec3 a = fabsf(n.x) > 0.9f ? v3(0,1,0) : v3(1,0,0);
    vec3 t = norm(cross(a, n)), b = cross(n, t);
    return norm(add(add(mul(t, cosf(r1)*s), mul(b, sinf(r1)*s)),
                    mul(n, sqrtf(1.0f - r2))));
}


static vec3 light_dir;   // direction to the light (norm)

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

// Ray/plane: solve dot(o + t*d - p, n) = 0. Two-sided. Returns t or -1.
static float hit_plane(const plane *pl, vec3 o, vec3 d) {
    float denom = dot(pl->n, d);
    if (fabsf(denom) < 1e-6f) return -1.0f;               // ray parallel to plane
    float t = dot(sub(pl->p, o), pl->n) / denom;
    if (t <= EPS) return -1.0f;                           // behind the ray / too close
 
    if (pl->hu > 0.0f) {                                  // bounded: check rectangle
        vec3 rel = sub(add(o, mul(d, t)), pl->p);
        if (fabsf(dot(rel, pl->u)) > pl->hu) return -1.0f;
        if (fabsf(dot(rel, pl->v)) > pl->hv) return -1.0f;
    }
    return t;
}

static int scene_hit(vec3 o, vec3 d, hit *h) {
    float best = 1e30f;
    int which = -1; // 0..NSPHERES-1 sphere, then planes
 
    for (int i = 0; i < NSPHERES; i++) {
        float t = hit_sphere(&spheres[i], o, d);
        if (t > 0.0f && t < best) { best = t; which = i; }
    }
    for (int i = 0; i < NPLANES; i++) {
        float t = hit_plane(&planes[i], o, d);
        if (t > 0.0f && t < best) { best = t; which = NSPHERES + i; }
    }
    if (which < 0) return 0;
 
    h->t = best;
    h->p = add(o, mul(d, best));
 
    if (which >= NSPHERES) {
        const plane *pl = &planes[which - NSPHERES];
        h->n = pl->n;
        h->m = pl->m;
        if (pl->tile > 0.0f) {
            vec3 rel = sub(h->p, pl->p);
            float inv = 1.0f / pl->tile;
            int chk = ((int)floorf(dot(rel, pl->u) * inv) + (int)floorf(dot(rel, pl->v) * inv)) & 1;
            if (chk) h->m.col = pl->col2;
        }
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
#define SUN 2.0f   // sun radiance (absorbs the 1/pi)

static vec3 trace(vec3 o, vec3 d, int maxdepth) {
    vec3 L = {0,0,0}, T = {1,1,1};

    for (int bounce = 0; bounce < maxdepth; bounce++) {
        hit h;
        if (!scene_hit(o, d, &h)) { L = add(L, vmul(T, sky(d))); break; }

        vec3 N = h.n;
        int entering = dot(N, d) < 0.0f;
        if (!entering) N = mul(N, -1.0f);
        material m = h.m;

        // Russian roulette: cheap unbiased early termination
        if (bounce > 3) {
            float p = fmaxf(T.x, fmaxf(T.y, T.z));
            if (p < 0.05f) p = 0.05f; if (p > 0.95f) p = 0.95f;
            if (rnd() > p) break;
            T = mul(T, 1.0f / p);
        }

        if (rnd() < m.transp) {                       // ---- glass
            float n1 = entering ? 1.0f : m.ior, n2 = entering ? m.ior : 1.0f;
            float cosi = -dot(N, d);
            vec3 tdir;
            int can = refract_dir(d, N, n1 / n2, &tdir);
            float R = 1.0f;
            if (can) {
                float r0 = (n1-n2)/(n1+n2); r0 *= r0;
                float c = (n1 <= n2) ? cosi : -dot(N, tdir);
                float k = 1.0f - c;
                R = r0 + (1.0f - r0) * k*k*k*k*k;
            }
            if (rnd() < R) { d = reflect_dir(d, N); o = add(h.p, mul(N,  EPS)); }
            else           { d = tdir;              o = sub(h.p, mul(N,  EPS)); }
            T = vmul(T, m.col);
        }
        else if (rnd() < m.refl) {                    // ---- mirror
            d = reflect_dir(d, N);
            o = add(h.p, mul(N, EPS));
        }
        else {                                        // ---- diffuse
            T = vmul(T, m.col);
            // next-event estimation: soft sun shadow via jittered light dir
            vec3 jit = v3(rnd()-0.5f, rnd()-0.5f, rnd()-0.5f);
            vec3 sd  = norm(add(light_dir, mul(jit, 0.15f)));
            float nl = dot(N, sd);
            if (nl > 0.0f) {
                float vis = shadow_factor(add(h.p, mul(N, EPS)), sd);
                L = add(L, mul(T, SUN * nl * vis));
            }
            d = cos_hemisphere(N);
            o = add(h.p, mul(N, EPS));
        }
    }
    return L;
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

static float accum[RW * RH * 3];

static void render_tile_pass(int x0, int y0, u32 seed, int spp) {
    const float px = 2.0f * 0.57735f / RH;
    const vec3 origin = {0,0,0};
    for (int y = y0; y < y0 + TILE; y++)
    for (int x = x0; x < x0 + TILE; x++) {
        rng_state = (seed ^ (x * 73856093u) ^ (y * 19349663u)) | 1u;
        for (int i = 0; i < 4; i++) rng_u32();           // warm up
        vec3 sum = {0,0,0};
        for (int s = 0; s < spp; s++) {
            float fx = x + rnd(), fy = y + rnd();        // random subpixel jitter
            vec3 d = norm(v3((fx - RW*0.5f)*px, (RH*0.5f - fy)*px, -1.0f));
            sum = add(sum, trace(origin, d, MAX_DEPTH));
        }
        float *a = &accum[(y*RW + x) * 3];
        a[0] += sum.x; a[1] += sum.y; a[2] += sum.z;
    }
}

static void resolve(int total_spp) {
    float inv = 1.0f / total_spp;
    for (int i = 0; i < RW*RH; i++)
        renderbuf[i] = pack_yuv(v3(sqrtf(accum[i*3]*inv),
                                   sqrtf(accum[i*3+1]*inv),
                                   sqrtf(accum[i*3+2]*inv)));
}

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

static inline u32 hash_u32(u32 x) {          // decent integer mixer (murmur3 finalizer)
    x ^= x >> 16; x *= 0x85ebca6bu;
    x ^= x >> 13; x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

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

    // start from a cleared accumulator and a black screen
    for (int i = 0; i < RW * RH * 3; i++) accum[i] = 0.0f;
    u32 black = pack_yuv(v3(0, 0, 0));
    for (int i = 0; i < RW * RH; i++) renderbuf[i] = black;
    present(xfb, W, H);

    const int tiles_x = RW / TILE;
    const int tiles_y = RH / TILE;
    const int ntiles  = tiles_x * tiles_y;

    int total_spp = 0;
    int pass = 0;

    for (;;) {
        // one pass = SPP_PER_JOB more samples for every tile
        for (int t = 0; t < ntiles; t++) {
            int tx = (t % tiles_x) * TILE;
            int ty = (t / tiles_x) * TILE;

            // job id -> seed. This is exactly what a coordinator would send to a worker.
            u32 job_id = (u32)pass * (u32)ntiles + (u32)t;
            u32 seed   = hash_u32(job_id + 1);

            render_tile_pass(tx, ty, seed, SPP_PER_JOB);

            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
        }

        total_spp += SPP_PER_JOB;
        pass++;

        resolve(total_spp);          // accum / total_spp -> gamma -> YUV
        present(xfb, W, H);
        VIDEO_WaitVSync();

        if (MAX_PASSES > 0 && pass >= MAX_PASSES) break;
    }

    // finished: hold the final image until HOME is pressed
    while (SYS_MainLoop()) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(0);
        VIDEO_WaitVSync();
    }
    return 0;
}