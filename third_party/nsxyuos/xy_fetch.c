/* Two things: what kind of file that is, and where NetSurf's own files live.
 *
 * The filetype question matters only for file: URLs -- everything off the
 * network carries a Content-Type and this is never consulted. So the answer
 * comes from the extension, which is all a name can tell anyone, and the
 * default is plain text rather than something that would be parsed as markup.
 *
 * The resources are the more important half. NetSurf keeps its own default
 * stylesheet, its quirks sheet and its about: pages behind resource: URLs and
 * asks the frontend to find them. Without default.css a page has no styling
 * at all -- not the page's styling, the browser's: no block layout, no
 * margins, no bold headings, because in CSS all of that comes from the user
 * agent stylesheet. A browser missing it does not look unstyled, it looks
 * broken. They live on the disc under /lib/netsurf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xy_front.h"

static const struct { const char *ext, *type; } by_extension[] = {
    { "html", "text/html" },        { "htm",  "text/html" },
    { "css",  "text/css" },
    { "txt",  "text/plain" },       { "text", "text/plain" },
    { "js",   "application/javascript" },
    { "json", "application/json" },
    { "xml",  "text/xml" },
    { "svg",  "image/svg+xml" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },       { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "bmp",  "image/bmp" },
    { "ico",  "image/x-icon" },
    { "webp", "image/webp" },
    { "pdf",  "application/pdf" },
    { "gz",   "application/gzip" },
    { "zip",  "application/zip" },
    { NULL, NULL }
};

static const char *xyf_filetype(const char *path) {
    if (path == NULL) return "text/plain";

    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot == NULL || (slash != NULL && dot < slash))
        return "text/plain";                 /* no extension to go on */

    for (int i = 0; by_extension[i].ext; i++)
        if (strcasecmp(dot + 1, by_extension[i].ext) == 0)
            return by_extension[i].type;

    return "text/plain";
}

/* --- where NetSurf's own files are --------------------------------------- */

#define RESOURCE_DIR "/lib/netsurf/"

/* Read one whole, and hand the bytes over. NetSurf holds them until it calls
 * release below, so there is no lifetime question to get wrong. */
static nserror xyf_get_resource_data(const char *path, const uint8_t **data,
                                     size_t *data_len) {
    if (path == NULL) return NSERROR_BAD_PARAMETER;

    char full[256];
    snprintf(full, sizeof full, RESOURCE_DIR "%s", path);

    FILE *f = fopen(full, "rb");
    if (f == NULL) return NSERROR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NSERROR_NOT_FOUND; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NSERROR_NOT_FOUND; }
    rewind(f);

    uint8_t *buf = malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NSERROR_NOMEM; }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;

    *data = buf;
    *data_len = got;
    return NSERROR_OK;
}

static nserror xyf_release_resource_data(const uint8_t *data) {
    free((void *)data);
    return NSERROR_OK;
}

static struct gui_fetch_table fetch_table = {
    .filetype              = xyf_filetype,
    .get_resource_data     = xyf_get_resource_data,
    .release_resource_data = xyf_release_resource_data,
};

struct gui_fetch_table *xy_fetch_table = &fetch_table;
