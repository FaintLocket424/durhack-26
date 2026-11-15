// Monte Carlo path tracer (mirror, glass w/ Schlick Fresnel, soft sun shadows).
// Same scene as the standalone tracer, but renders arbitrary tiles of an
// arbitrary-resolution image and returns raw sample sums.
#include <math.h>
#include <string.h>

#include "render.h"

#define EPS   1e-3f
#define SUN   2.0f

typedef uint32_t u32;

// ---------------------------------------------------------------- vec3
typedef struct { float x, y, z; } vec3;

static inline vec3  v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3  add(vec3 a, vec3 b)   { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3  sub(vec3 a, vec3 b)   { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3  mul(vec3 a, float s)  { return v3(a.x*s, a.y*s, a.z*s); }
static inline vec3  vmul(vec3 a, vec3 b)  { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline float dot(vec3 a, vec3 b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3  norm(vec3 a)          { return mul(a, 1.0f / sqrtf(dot(a, a))); }
static inline vec3  cross(vec3 a, vec3 b) { return v3(a.y*b.z - a.z*b.y,
                                                      a.z*b.x - a.x*b.z,
                                                      a.x*b.y - a.y*b.x); }

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline vec3 reflect_dir(vec3 d, vec3 n) {
    return sub(d, mul(n, 2.0f * dot(d, n)));
}

// Snell's law. n must face the incoming ray. Returns 0 on total internal reflection.
static inline int refract_dir(vec3 d, vec3 n, float eta, vec3 *out) {
    float cosi = -dot(n, d);
    float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
    if (k < 0.0f) return 0;
    *out = add(mul(d, eta), mul(n, eta * cosi - sqrtf(k)));
    return 1;
}

// ---------------------------------------------------------------- random
static u32 rng_state = 1;

static inline u32 rng_u32(void) {            // xorshift32
    u32 x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static inline float rnd(void) {              // [0, 1)
    return (rng_u32() >> 8) * (1.0f / 16777216.0f);
}

static inline u32 hash_u32(u32 x) {          // murmur3 finalizer
    x ^= x >> 16; x *= 0x85ebca6bu;
    x ^= x >> 13; x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

static vec3 cos_hemisphere(vec3 n) {
    float r1 = 6.2831853f * rnd(), r2 = rnd(), s = sqrtf(r2);
    vec3 a = fabsf(n.x) > 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
    vec3 t = norm(cross(a, n)), b = cross(n, t);
    return norm(add(add(mul(t, cosf(r1) * s), mul(b, sinf(r1) * s)),
                    mul(n, sqrtf(1.0f - r2))));
}

// ---------------------------------------------------------------- scene
typedef struct {
    vec3  col;
    float refl;     // mirror weight for opaque materials
    float transp;   // chance of taking the glass path
    float ior;
} material;

typedef struct { vec3 c; float r; material m; } sphere;

static const sphere spheres[] = {
    {{ 0.0f,  0.0f, -5.0f}, 1.0f, {{0.90f, 0.95f, 1.00f}, 0.0f,  0.90f, 1.52f}},   // glass
    {{ 2.2f, -0.4f, -6.0f}, 0.6f, {{0.20f, 0.80f, 0.30f}, 0.9f,  0.0f,  1.0f }},   // mirror
    {{-2.0f, -0.3f, -4.5f}, 0.7f, {{0.25f, 0.35f, 0.90f}, 0.25f, 0.35f, 1.5f }},   // glossy blue
    {{ 0.3f,  0.0f, -8.5f}, 1.0f, {{0.90f, 0.50f, 0.20f}, 0.5f,  0.0f,  1.0f }},   // orange
};
#define NSPHERES ((int)(sizeof(spheres) / sizeof(spheres[0])))

typedef struct {
    vec3  p, n, u, v;
    float hu, hv;          // half extents; hu <= 0 = infinite
    float tile;            // > 0: checkerboard of m.col / col2
    vec3  col2;
    material m;
} plane;

static plane planes[] = {
    { {0, -1, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}, 0.0f, 0.0f,
      1.0f, {0.125f, 0.125f, 0.125f}, {{0.85f, 0.85f, 0.85f}, 0.25f, 0.0f, 1.0f} },

    { {3.5f, 1.0f, -9.0f}, {-1, 0, 1}, {1, 0, 1}, {0, 0, 0}, 5.0f, 2.0f,
      0.75f, {0.05f, 0.05f, 0.05f}, {{0.05f, 0.05f, 0.05f}, 0.35f, 0.0f, 1.0f} },
};
#define NPLANES ((int)(sizeof(planes) / sizeof(planes[0])))

static vec3 light_dir;
static int  scene_ready = 0;

static void init_scene(void) {
    if (scene_ready) return;
    for (int i = 0; i < NPLANES; i++) {
        plane *pl = &planes[i];
        pl->n = norm(pl->n);
        pl->u = norm(sub(pl->u, mul(pl->n, dot(pl->u, pl->n))));
        pl->v = cross(pl->n, pl->u);
    }
    light_dir = norm(v3(-0.6f, 1.0f, 0.4f));
    scene_ready = 1;
}

// ---------------------------------------------------------------- intersection
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
    t = -b + sq;
    return (t > EPS) ? t : -1.0f;
}

static float hit_plane(const plane *pl, vec3 o, vec3 d) {
    float denom = dot(pl->n, d);
    if (fabsf(denom) < 1e-6f) return -1.0f;
    float t = dot(sub(pl->p, o), pl->n) / denom;
    if (t <= EPS) return -1.0f;

    if (pl->hu > 0.0f) {
        vec3 rel = sub(add(o, mul(d, t)), pl->p);
        if (fabsf(dot(rel, pl->u)) > pl->hu) return -1.0f;
        if (fabsf(dot(rel, pl->v)) > pl->hv) return -1.0f;
    }
    return t;
}

static int scene_hit(vec3 o, vec3 d, hit *h) {
    float best = 1e30f;
    int which = -1;

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
            int chk = ((int)floorf(dot(rel, pl->u) * inv) +
                       (int)floorf(dot(rel, pl->v) * inv)) & 1;
            if (chk) h->m.col = pl->col2;
        }
    } else {
        h->n = mul(sub(h->p, spheres[which].c), 1.0f / spheres[which].r);
        h->m = spheres[which].m;
    }
    return 1;
}

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

// ---------------------------------------------------------------- path trace
static vec3 trace(vec3 o, vec3 d, int maxdepth) {
    vec3 L = {0, 0, 0};
    vec3 T = {1, 1, 1};

    for (int bounce = 0; bounce < maxdepth; bounce++) {
        hit h;
        if (!scene_hit(o, d, &h)) {
            L = add(L, vmul(T, sky(d)));
            break;
        }

        vec3 N = h.n;
        int entering = dot(N, d) < 0.0f;
        if (!entering) N = mul(N, -1.0f);
        material m = h.m;

        if (bounce > 3) {                              // Russian roulette
            float p = clampf(fmaxf(T.x, fmaxf(T.y, T.z)), 0.05f, 0.95f);
            if (rnd() > p) break;
            T = mul(T, 1.0f / p);
        }

        if (rnd() < m.transp) {                        // glass
            float n1 = entering ? 1.0f : m.ior;
            float n2 = entering ? m.ior : 1.0f;
            float cosi = -dot(N, d);
            vec3 tdir;
            int can_refract = refract_dir(d, N, n1 / n2, &tdir);
            float R = 1.0f;
            if (can_refract) {
                float r0 = (n1 - n2) / (n1 + n2);
                r0 *= r0;
                float c = (n1 <= n2) ? cosi : -dot(N, tdir);
                float k = 1.0f - c;
                R = r0 + (1.0f - r0) * k*k*k*k*k;
            }
            if (rnd() < R) { d = reflect_dir(d, N); o = add(h.p, mul(N, EPS)); }
            else           { d = tdir;              o = sub(h.p, mul(N, EPS)); }
            T = vmul(T, m.col);
        }
        else if (rnd() < m.refl) {                     // mirror
            d = reflect_dir(d, N);
            o = add(h.p, mul(N, EPS));
        }
        else {                                         // diffuse + sun NEE
            T = vmul(T, m.col);
            vec3 jit = v3(rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f);
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

// ---------------------------------------------------------------- tiles
static render_progress_fn progress_fn = NULL;
static void *progress_user = NULL;

void render_set_progress(render_progress_fn fn, void *user) {
    progress_fn = fn;
    progress_user = user;
}

int render_tile(const tile_job_t *job, float *accum) {
    init_scene();

    const float px = 2.0f * 0.57735f / (float)job->full_h;   // 60 degree vertical FOV
    const float cx = job->full_w * 0.5f;
    const float cy = job->full_h * 0.5f;
    const vec3 origin = {0, 0, 0};

    for (u32 pass = 0; pass < job->spp; pass++) {
        for (u32 ty = 0; ty < job->h; ty++) {
            for (u32 tx = 0; tx < job->w; tx++) {
                u32 gx = job->x0 + tx, gy = job->y0 + ty;    // global pixel

                // Seed depends on request seed, pass and GLOBAL pixel, so any
                // tile split gives the same noise for the same seed.
                rng_state = hash_u32(job->seed ^ (pass * 0x9E3779B1u)
                                     ^ (gx * 73856093u) ^ (gy * 19349663u)) | 1u;
                for (int i = 0; i < 4; i++) rng_u32();

                float fx = gx + rnd(), fy = gy + rnd();
                vec3 d = norm(v3((fx - cx) * px, (cy - fy) * px, -1.0f));
                vec3 c = trace(origin, d, (int)job->max_depth);

                float *a = &accum[(ty * job->w + tx) * 3];
                a[0] += c.x;
                a[1] += c.y;
                a[2] += c.z;
            }
        }
        if (progress_fn && progress_fn(job, accum, pass + 1, progress_user))
            return -1;
    }
    return 0;
}
