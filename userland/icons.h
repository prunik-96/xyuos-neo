#ifndef XYUOS_ICONS_H
#define XYUOS_ICONS_H

/* The icon set, for programs that draw with gui.h.
 *
 * Same blob the kernel reads (/icons.bin), same two sizes. Loading it is
 * optional in the strictest sense: every one of these calls reports failure
 * plainly, and every caller is expected to keep the drawing it had before.
 * An icon set is decoration, and decoration must never be load-bearing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"

#define ICON_BIG   32
#define ICON_SMALL 16
#define ICON_NAME  24

typedef struct {
    unsigned char *blob;
    unsigned       count;
    unsigned char *index;      /* count * (24 + 4 + 4) */
    unsigned char *pixels;
} iconset_t;

static inline int icons_open(iconset_t *is) {
    memset(is, 0, sizeof *is);
    int fd = open("/icons.bin", 0);
    if (fd < 0) return 0;

    /* Read it whole: it is small, and a partial icon set is worse than none. */
    unsigned cap = 512 * 1024, got = 0;
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) { close(fd); return 0; }
    for (;;) {
        long n = read(fd, b + got, cap - got);
        if (n <= 0) break;
        got += (unsigned)n;
        if (got >= cap) break;
    }
    close(fd);

    if (got < 16 || b[0] != 'X' || b[1] != 'I' || b[2] != 'C' || b[3] != 'O') {
        free(b);
        return 0;
    }
    unsigned *hdr = (unsigned *)(b + 4);
    if (hdr[1] != ICON_BIG || hdr[2] != ICON_SMALL) { free(b); return 0; }

    is->blob   = b;
    is->count  = hdr[0];
    is->index  = b + 16;
    is->pixels = b + 16 + is->count * 32;
    return 1;
}

/* The pixels for `name` as 0xAARRGGBB, or NULL. */
static inline const unsigned *icon_find(const iconset_t *is,
                                        const char *name, int size) {
    if (!is->blob || !name) return 0;
    for (unsigned i = 0; i < is->count; i++) {
        const char *n = (const char *)(is->index + i * 32);
        if (strncmp(n, name, ICON_NAME) != 0) continue;
        unsigned *offs = (unsigned *)(is->index + i * 32 + ICON_NAME);
        unsigned off = (size == ICON_SMALL) ? offs[1] : offs[0];
        return (const unsigned *)(is->pixels + off);
    }
    return 0;
}

/* Blend an icon into the surface. Returns 0 when there is no such icon, so
 * the caller can draw whatever it drew before. */
static inline int icon_blit(gui_t *g, int x, int y, const iconset_t *is,
                            const char *name, int size) {
    const unsigned *src = icon_find(is, name, size);
    if (!src || !g->px) return 0;
    for (int j = 0; j < size; j++) {
        int py = y + j;
        if (py < 0 || py >= g->h) continue;
        for (int i = 0; i < size; i++) {
            int px = x + i;
            if (px < 0 || px >= g->w) continue;
            unsigned s = src[j * size + i];
            unsigned a = s >> 24;
            if (!a) continue;
            unsigned *d = &g->px[py * g->w + px];
            if (a == 255) { *d = s & 0xFFFFFF; continue; }
            unsigned o = *d;
            unsigned r = (((s >> 16) & 0xFF) * a + ((o >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned gg = (((s >> 8) & 0xFF) * a + ((o >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned b = ((s & 0xFF) * a + (o & 0xFF) * (255 - a)) / 255;
            *d = (r << 16) | (gg << 8) | b;
        }
    }
    return 1;
}

#endif
