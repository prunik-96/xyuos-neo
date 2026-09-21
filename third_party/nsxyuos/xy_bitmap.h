/* The frontend's bitmap: what NetSurf's image decoders draw into. */
#ifndef XY_BITMAP_H
#define XY_BITMAP_H

#include <stdbool.h>
#include <stddef.h>

/* NetSurf writes these as four bytes per pixel in R, G, B, A order -- see
 * pixel_to_colour in netsurf/plot_style.h. The surface wants ARGB in a word,
 * so the conversion happens where the bitmap is drawn, not here: the decoders
 * write far more pixels than get shown. */
struct xy_bitmap {
    int            w, h;
    size_t         stride;
    bool           opaque;
    unsigned char *data;
};

#endif
