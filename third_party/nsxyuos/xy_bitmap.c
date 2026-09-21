/* The bitmap table: somewhere for NetSurf's image decoders to put pixels.
 *
 * Nothing clever is wanted here. NetSurf asks for a block of R,G,B,A bytes,
 * writes an image into it, and later asks the plotters to draw it. The only
 * decision worth making is what "opaque" means, and NetSurf asks that
 * question itself through test_opaque -- answering it truthfully lets the
 * bitmap plotter skip per-pixel blending for the photographs, which is most
 * of them.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"

#include "xy_front.h"
#include "xy_bitmap.h"

static void *xyb_create(int width, int height, unsigned int state) {
    if (width <= 0 || height <= 0) return NULL;
    /* A page can name an enormous image; refusing is better than failing to
     * allocate somewhere less prepared for it. */
    if ((long)width * height > 32L * 1024 * 1024) return NULL;

    struct xy_bitmap *bm = calloc(1, sizeof *bm);
    if (bm == NULL) return NULL;

    bm->w = width;
    bm->h = height;
    bm->stride = (size_t)width * 4;
    bm->opaque = (state & BITMAP_OPAQUE) != 0;

    size_t bytes = bm->stride * (size_t)height;
    bm->data = (state & BITMAP_CLEAR_MEMORY) ? calloc(1, bytes) : malloc(bytes);
    if (bm->data == NULL) { free(bm); return NULL; }

    return bm;
}

static void xyb_destroy(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    if (bm == NULL) return;
    free(bm->data);
    free(bm);
}

static void xyb_set_opaque(void *bitmap, bool opaque) {
    struct xy_bitmap *bm = bitmap;
    if (bm) bm->opaque = opaque;
}

static bool xyb_get_opaque(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    return bm ? bm->opaque : false;
}

/* Whether every pixel really is opaque. Worth the pass: an answer of yes
 * takes the blend out of the inner loop for the whole image. */
static bool xyb_test_opaque(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    if (bm == NULL) return false;
    for (int y = 0; y < bm->h; y++) {
        const unsigned char *row = bm->data + (size_t)y * bm->stride;
        for (int x = 0; x < bm->w; x++)
            if (row[x * 4 + 3] != 0xFF) return false;
    }
    return true;
}

static unsigned char *xyb_get_buffer(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    return bm ? bm->data : NULL;
}

static size_t xyb_get_rowstride(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    return bm ? bm->stride : 0;
}

static int xyb_get_width(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    return bm ? bm->w : 0;
}

static int xyb_get_height(void *bitmap) {
    struct xy_bitmap *bm = bitmap;
    return bm ? bm->h : 0;
}

static size_t xyb_get_bpp(void *bitmap) {
    (void)bitmap;
    return 4;
}

static bool xyb_save(void *bitmap, const char *path, unsigned flags) {
    (void)bitmap; (void)path; (void)flags;
    /* Writing a picture file needs an encoder, and this system has decoders.
     * Saying no is the truth; the caller is a "save image as" menu entry
     * that does not exist yet either. */
    return false;
}

static void xyb_modified(void *bitmap) {
    (void)bitmap;   /* nothing is cached about it that could go stale */
}

/* Draw a content into a bitmap -- what a thumbnail is. */
static nserror xyb_render(struct bitmap *bitmap, struct hlcache_handle *content) {
    (void)bitmap; (void)content;
    return NSERROR_NOT_IMPLEMENTED;
}

static struct gui_bitmap_table bitmap_table = {
    .create        = xyb_create,
    .destroy       = xyb_destroy,
    .set_opaque    = xyb_set_opaque,
    .get_opaque    = xyb_get_opaque,
    .test_opaque   = xyb_test_opaque,
    .get_buffer    = xyb_get_buffer,
    .get_rowstride = xyb_get_rowstride,
    .get_width     = xyb_get_width,
    .get_height    = xyb_get_height,
    .get_bpp       = xyb_get_bpp,
    .save          = xyb_save,
    .modified      = xyb_modified,
    .render        = xyb_render,
};

struct gui_bitmap_table *xy_bitmap_table = &bitmap_table;
