#ifndef NODE_H
#define NODE_H

#include <gccore.h>

typedef struct { float x, y, z; } vec3;

static void init_scene(void);

static void present(u32 *xfb, int W, int H);
static void render_tile(int x0, int y0);

#endif