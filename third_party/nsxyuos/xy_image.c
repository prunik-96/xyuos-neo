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
 * TWO LIMITATIONS, both from the decoder rather than from here:
 *
 *   There is no GIF. img_load does not recognise one, so an animated banner
 *   or an old spacer image will not appear. Everything else on a modern page
 *   is PNG, JPEG or SVG.
 *
 *   Transparency is flattened at decode time against img_background, because
 *   image_t has no alpha channel -- one word per pixel, 0x00RRGGBB. A
 *   transparent PNG therefore arrives already composited onto a colour
 *   chosen before NetSurf said what was behind it. White is the right guess
 *   for most pages and the wrong one for a dark design, where the edges of a
 *   logo will show a pale halo. Fixing it properly means giving image_t an
 *   alpha channel, which is a change to the picture viewer as well.
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
} xy_image_content;

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

    image_t pic;
    if (!img_load(data, (unsigned long)size, &pic) || pic.px == NULL) {
        /* img_err says which of the formats it tried and why it stopped. */
        content_broadcast_errorcode(c, NSERROR_INVALID);
        return false;
    }

    im->bitmap = guit->bitmap->create(pic.w, pic.h, BITMAP_NEW | BITMAP_OPAQUE);
    if (im->bitmap == NULL) {
        img_free(&pic);
        content_broadcast_errorcode(c, NSERROR_NOMEM);
        return false;
    }

    /* One word per pixel becomes four bytes per pixel. NetSurf reads a
     * bitmap as R, G, B, A in that order (see pixel_to_colour in
     * netsurf/plot_style.h); this decoder hands back 0x00RRGGBB in a word.
     * Alpha is 255 throughout because the decoder has already flattened it. */
    unsigned char *out = guit->bitmap->get_buffer(im->bitmap);
    size_t stride = guit->bitmap->get_rowstride(im->bitmap);

    for (int y = 0; y < pic.h; y++) {
        const unsigned int *src = pic.px + (size_t)y * pic.w;
        unsigned char *dst = out + (size_t)y * stride;
        for (int x = 0; x < pic.w; x++) {
            unsigned int v = src[x];
            *dst++ = (unsigned char)((v >> 16) & 0xFF);   /* R */
            *dst++ = (unsigned char)((v >> 8) & 0xFF);    /* G */
            *dst++ = (unsigned char)(v & 0xFF);           /* B */
            *dst++ = 0xFF;                                /* A */
        }
    }

    c->width = pic.w;
    c->height = pic.h;
    c->size += (size_t)pic.w * pic.h * 4;

    guit->bitmap->set_opaque(im->bitmap, true);
    guit->bitmap->modified(im->bitmap);
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
