/* Pictures, decoded by the decoders this system already had.
 *
 * NetSurf ships handlers for PNG, JPEG, GIF and the rest, and each of them
 * wants a library: libpng, libjpeg, libnsgif. None of those are here. What is
 * here is userland/img.h, written for this system's own picture viewer and
 * browser, which decodes PNG, JPEG, BMP, WebP and SVG from the file format up.
 *
 * So this is one handler instead of five. NetSurf's content handlers are
 * registered by MIME type and each of its own is a few hundred lines of the
 * same shape around a different library; there is no reason to write that
 * five times when the decoder underneath is a single call that sniffs the
 * magic bytes and dispatches.
 *
 * There is no GIF: img_load does not recognise one, so an animated banner or
 * an old spacer image will not appear. Everything else on a modern page is
 * PNG, JPEG, WebP or SVG.
 *
 * Transparency comes through. The decoders lay a picture on img_background
 * and keep its coverage alongside; laid on black, what they hand back is the
 * colour already multiplied by its coverage, so dividing it out again gives
 * NetSurf the true colour with its alpha -- and a logo on a dark page is
 * drawn on that page instead of on a white square.
 *
 * An SVG is a drawing, not pixels, so it is drawn again at whatever size the
 * page shows it: a logo scaled to fit its box is as sharp as one drawn to
 * size, because it is one.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/utils.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"

#include "xy_front.h"
#include "xy_bitmap.h"
#include "xy_image.h"

/* Down here: img.h defines a great many static functions, and NetSurf's
 * headers should be read before them. */
#include "img.h"

typedef struct {
    struct content base;
    struct bitmap *bitmap;
    bool           svg;         /* redrawn at the size it is shown */
    int            bw, bh;      /* the size the bitmap was made at */
} xy_image_content;

/* A decoded picture into a NetSurf bitmap, replacing the one there was.
 * NetSurf reads a bitmap as R, G, B, A (see pixel_to_colour in
 * netsurf/plot_style.h). */
static bool xyi_fill(xy_image_content *im, const image_t *pic) {
    bool opaque = pic->alpha == NULL;
    struct bitmap *bm = guit->bitmap->create(pic->w, pic->h,
                                             BITMAP_NEW | (opaque ? BITMAP_OPAQUE : 0));
    if (bm == NULL) return false;

    unsigned char *out = guit->bitmap->get_buffer(bm);
    size_t stride = guit->bitmap->get_rowstride(bm);
    for (int y = 0; y < pic->h; y++) {
        const unsigned int *src = pic->px + (size_t)y * pic->w;
        const unsigned char *al = opaque ? NULL : pic->alpha + (size_t)y * pic->w;
        unsigned char *dst = out + (size_t)y * stride;
        for (int x = 0; x < pic->w; x++) {
            unsigned int v = src[x], r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            unsigned int a = al ? al[x] : 255;
            if (a == 0) {
                r = g = b = 0;
            } else if (a < 255) {
                /* laid on black: the colour times its coverage */
                r = (r * 255 + a / 2) / a; if (r > 255) r = 255;
                g = (g * 255 + a / 2) / a; if (g > 255) g = 255;
                b = (b * 255 + a / 2) / a; if (b > 255) b = 255;
            }
            *dst++ = (unsigned char)r;
            *dst++ = (unsigned char)g;
            *dst++ = (unsigned char)b;
            *dst++ = (unsigned char)a;
        }
    }
    guit->bitmap->set_opaque(bm, opaque);
    guit->bitmap->modified(bm);

    if (im->bitmap != NULL) guit->bitmap->destroy(im->bitmap);
    im->bitmap = bm;
    im->bw = pic->w;
    im->bh = pic->h;
    return true;
}

static nserror xyi_create(const content_handler *handler,
                          lwc_string *imime_type,
                          const struct http_parameter *params,
                          llcache_handle *llcache,
                          const char *fallback_charset, bool quirks,
                          struct content **c) {
    xy_image_content *im = calloc(1, sizeof *im);
    if (im == NULL) return NSERROR_NOMEM;

    nserror e = content__init(&im->base, handler, imime_type, params,
                              llcache, fallback_charset, quirks);
    if (e != NSERROR_OK) { free(im); return e; }

    *c = (struct content *)im;
    return NSERROR_OK;
}

/* The whole file has arrived: decode it once, here, rather than at every
 * redraw. An image is drawn far more often than it is fetched. */
static bool xyi_convert(struct content *c) {
    xy_image_content *im = (xy_image_content *)c;

    size_t size = 0;
    const uint8_t *data = (const uint8_t *)content__get_source_data(c, &size);
    if (data == NULL || size == 0) {
        content_broadcast_errorcode(c, NSERROR_INVALID);
        return false;
    }

    /* On black, so that what comes back with an alpha plane is the colour
     * multiplied by it -- see xyi_fill. */
    img_background = 0x000000;
    image_t pic;
    if (!img_load(data, (unsigned long)size, &pic) || pic.px == NULL) {
        /* img_err says which of the formats it tried and why it stopped. */
        content_broadcast_errorcode(c, NSERROR_INVALID);
        return false;
    }
    if (!xyi_fill(im, &pic)) {
        img_free(&pic);
        content_broadcast_errorcode(c, NSERROR_NOMEM);
        return false;
    }

    /* A drawing's size is what it says it is, not the larger size it was
     * rendered at for sharpness; the page is laid out with the former. */
    int w = pic.w, h = pic.h;
    bool binary = data[0] == 137 || data[0] == 0xFF || (data[0] == 'B' && data[1] == 'M') ||
                  (size > 4 && data[0] == 'R' && data[1] == 'I');
    im->svg = !binary && svg_size(data, (unsigned long)size, &w, &h) != 0;
    c->width = w;
    c->height = h;
    c->size += (size_t)pic.w * pic.h * 4;
    img_free(&pic);

    char *title = messages_get_buff("BMPTitle",
            nsurl_access_leaf(llcache_handle_get_url(c->llcache)),
            c->width, c->height);
    if (title != NULL) {
        content__set_title(c, title);
        free(title);
    }

    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    return true;
}

static bool xyi_redraw(struct content *c, struct content_redraw_data *data,
                       const struct rect *clip,
                       const struct redraw_context *ctx) {
    (void)clip;
    xy_image_content *im = (xy_image_content *)c;
    if (im->bitmap == NULL) return false;

    /* A drawing shown at a size it was not drawn at is drawn again at that
     * size. Only one size is kept: the same picture shown at two sizes on
     * one page is drawn twice per redraw, which is rare and still cheap. */
    if (im->svg && data->width > 0 && data->height > 0 &&
        data->width <= 2048 && data->height <= 2048 &&
        (data->width != im->bw || data->height != im->bh)) {
        size_t size = 0;
        const uint8_t *src = (const uint8_t *)content__get_source_data(c, &size);
        image_t pic;
        img_background = 0x000000;
        if (src != NULL && size > 0 &&
            svg_render(src, (unsigned long)size, data->width, data->height, &pic)) {
            xyi_fill(im, &pic);
            img_free(&pic);
        }
    }

    bitmap_flags_t flags = BITMAPF_NONE;
    if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
    if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;

    return ctx->plot->bitmap(ctx, im->bitmap, data->x, data->y,
                             data->width, data->height,
                             data->background_colour, flags) == NSERROR_OK;
}

static void xyi_destroy(struct content *c) {
    xy_image_content *im = (xy_image_content *)c;
    if (im->bitmap != NULL) {
        guit->bitmap->destroy(im->bitmap);
        im->bitmap = NULL;
    }
}

static nserror xyi_clone(const struct content *old, struct content **newc) {
    xy_image_content *im = calloc(1, sizeof *im);
    if (im == NULL) return NSERROR_NOMEM;

    nserror e = content__clone(old, &im->base);
    if (e != NSERROR_OK) { content_destroy(&im->base); return e; }

    /* Cloned by replaying the decode, which is what every one of NetSurf's
     * own image handlers does: the bitmap is not shareable between contents. */
    if (old->status == CONTENT_STATUS_READY ||
        old->status == CONTENT_STATUS_DONE) {
        if (!xyi_convert(&im->base)) {
            content_destroy(&im->base);
            return NSERROR_CLONE_FAILED;
        }
    }

    *newc = (struct content *)im;
    return NSERROR_OK;
}

static void *xyi_get_internal(const struct content *c, void *context) {
    (void)context;
    return ((xy_image_content *)c)->bitmap;
}

static content_type xyi_content_type(void) { return CONTENT_IMAGE; }

static const content_handler xy_image_handler = {
    .create        = xyi_create,
    .data_complete = xyi_convert,
    .destroy       = xyi_destroy,
    .redraw        = xyi_redraw,
    .clone         = xyi_clone,
    .get_internal  = xyi_get_internal,
    .type          = xyi_content_type,
    .no_share      = false,
};

/* Every type the decoder can actually read, and the aliases servers really
 * send. What it cannot read -- GIF above all -- is deliberately not listed:
 * claiming a type and then failing is worse than not claiming it, because
 * NetSurf would stop looking for another way to show it. */
static const char *xy_image_types[] = {
    "image/png",       "image/x-png",       "application/png",
    "image/jpeg",      "image/jpg",         "image/pjpeg",
    "image/bmp",       "image/x-bmp",       "image/x-ms-bmp",
    "image/ms-bmp",    "application/bmp",   "application/x-bmp",
    "image/webp",
    "image/svg+xml",   "image/svg",
    /* A favicon is usually a PNG whatever it is called, and the decoder
     * looks at the bytes rather than the name. */
    "image/x-icon",    "image/vnd.microsoft.icon",
};

CONTENT_FACTORY_REGISTER_TYPES(xy_image, xy_image_types, xy_image_handler);
