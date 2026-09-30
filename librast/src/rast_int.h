#ifndef RAST_INT_H
#define RAST_INT_H

/* What the pieces of librast share and nobody else needs. */

#include "rast.h"
#include <stdlib.h>
#include <string.h>

/* A path after flattening: polylines, one per subpath. Coordinates are 16.16
 * in whatever space the flattening was asked for. */
typedef struct {
    int32_t *xy;               /* x,y pairs */
    int      n, cap;           /* points */
    int     *start;            /* index of each subpath's first point */
    uint8_t *closed;
    int      ns, scap;
    int      err;
} rpoly;

void rpoly_init(rpoly *P);
void rpoly_free(rpoly *P);
void rpoly_reset(rpoly *P);
void rpoly_move(rpoly *P, int32_t x, int32_t y);
void rpoly_line(rpoly *P, int32_t x, int32_t y);    /* skips a repeat of the last */
void rpoly_close(rpoly *P);
int  rpoly_len(const rpoly *P, int s);               /* points in subpath s */

/* Flatten p through m (NULL: none) to within `tol` (16.16, output space). */
void rast_flatten(const rast_path *p, const rast_mat *m, rast_fx tol, rpoly *out);

/* The stroke outline of `in` (flattened, in the path's space) as closed
 * polygons to be filled nonzero. `tol` bounds the error of round parts. */
void rast_stroke_outline(const rpoly *in, const rast_stroke *s, rast_fx tol,
                         rpoly *out);

/* The dashed version of `in`: every "on" stretch as an open subpath. */
void rast_dash(const rpoly *in, const rast_stroke *s, rpoly *out);

/* Device-space flattening tolerance: a tenth of a pixel. The chords of a
 * flattened curve lie inside it, so the tolerance is also how much a shape
 * shrinks at its curved edges -- at a fifth of a pixel a 20-pixel dot came
 * out 2.5% short of its area, which is a visibly thinner line. */
#define RAST_TOL (RAST_ONE / 10)

static inline int32_t rast_clamp32(int64_t v) {
    if (v > 0x3FFFFFFF) return 0x3FFFFFFF;
    if (v < -0x3FFFFFFF) return -0x3FFFFFFF;
    return (int32_t)v;
}

/* Length of (dx,dy), both 16.16, as 16.16. */
static inline rast_fx rast_len(rast_fx dx, rast_fx dy) {
    uint64_t s = (uint64_t)((int64_t)dx * dx) + (uint64_t)((int64_t)dy * dy);
    return (rast_fx)rast_isqrt64(s);
}

#endif
