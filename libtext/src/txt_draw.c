/* Drawing: glyphs into pixels, and a cache so each is drawn by FreeType once.
 *
 * HarfBuzz places glyphs at fractions of a pixel, and rounding every one of
 * them to a whole pixel makes the spacing of a word visibly uneven -- "rn"
 * that looks like "m" in one place and not in another. So each glyph is
 * rendered at the quarter pixel it actually falls on: four versions at most
 * of any glyph at any size, which is what the cache is for.
 *
 * Hinting is FreeType's light mode: outlines are snapped to the pixel grid
 * vertically only, so stems and baselines are crisp and the letters keep the
 * widths HarfBuzz measured them at. Full hinting would change the widths
 * and break exactly the agreement between measuring and drawing that all of
 * libtext is built around.
 */
#include <stdlib.h>
#include <string.h>

#include "txt_internal.h"
#include FT_OUTLINE_H

typedef struct {
    int      size;              /* 0 marks an empty slot                   */
    unsigned gid;
    short    face;
    unsigned char phase, synth;
    short    left, top, w, h;
    unsigned char *bits;        /* w * h coverage, 0..255                  */
} glyph_t;

#define GCACHE 8192             /* a power of two                          */
#define GBYTES (24u << 20)      /* when the bitmaps pass this, start again */

static glyph_t cache[GCACHE];
static int     used;
static size_t  bytes;

static void flush(void) {
    for (int i = 0; i < GCACHE; i++) {
        free(cache[i].bits);
        cache[i].bits = NULL;
        cache[i].size = 0;
    }
    used = 0;
    bytes = 0;
}

static void render(glyph_t *g, txt_face *f, int bold, int italic) {
    g->left = g->top = g->w = g->h = 0;
    g->bits = NULL;
    if (f->ft_size != g->size) {
        if (FT_Set_Char_Size(f->ft, 0, g->size, 72, 72)) return;
        f->ft_size = g->size;
    }
    if (FT_Load_Glyph(f->ft, g->gid, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT) &&
        FT_Load_Glyph(f->ft, g->gid, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING))
        return;
    FT_GlyphSlot sl = f->ft->glyph;
    if (sl->format != FT_GLYPH_FORMAT_OUTLINE) return;

    if (bold) {
        /* A twenty-fourth of the em, as FreeType's own emboldening uses. */
        FT_Pos s = g->size / 24;
        FT_Outline_EmboldenXY(&sl->outline, s, s);
    }
    if (italic) {
        FT_Matrix m = { 0x10000, 0x0366A, 0, 0x10000 };   /* about 12 degrees */
        FT_Outline_Transform(&sl->outline, &m);
    }
    if (g->phase) FT_Outline_Translate(&sl->outline, g->phase * 16, 0);
    if (FT_Render_Glyph(sl, FT_RENDER_MODE_NORMAL)) return;

    FT_Bitmap *b = &sl->bitmap;
    if (b->pixel_mode != FT_PIXEL_MODE_GRAY || !b->width || !b->rows) return;
    unsigned char *bits = malloc((size_t)b->width * b->rows);
    if (!bits) return;
    for (unsigned y = 0; y < b->rows; y++)
        memcpy(bits + (size_t)y * b->width,
               b->buffer + (ptrdiff_t)y * b->pitch, b->width);
    g->bits = bits;
    g->w = (short)b->width;
    g->h = (short)b->rows;
    g->left = (short)sl->bitmap_left;
    g->top = (short)sl->bitmap_top;
    bytes += (size_t)b->width * b->rows;
}

static const glyph_t *glyph(int face, unsigned gid, int size, int phase,
                            int bold, int italic) {
    unsigned synth = (unsigned)(bold | italic << 1);
    unsigned h = (gid * 2654435761u) ^ ((unsigned)face * 97u) ^
                 ((unsigned)size * 40503u) ^ ((unsigned)phase << 20) ^ (synth << 24);
    for (int probe = 0; probe < GCACHE; probe++) {
        glyph_t *g = &cache[(h + (unsigned)probe) & (GCACHE - 1)];
        if (g->size == 0) {
            if (used >= GCACHE * 3 / 4 || bytes > GBYTES) {
                flush();
                return glyph(face, gid, size, phase, bold, italic);
            }
            txt_face *f = txt_face_get(face);
            if (!f) return NULL;
            g->size = size;
            g->gid = gid;
            g->face = (short)face;
            g->phase = (unsigned char)phase;
            g->synth = (unsigned char)synth;
            render(g, f, bold, italic);
            used++;
            return g;
        }
        if (g->size == size && g->gid == gid && g->face == face &&
            g->phase == phase && g->synth == synth)
            return g;
    }
    return NULL;
}

static void blit(const txt_target *t, const glyph_t *g, int x, int y,
                 uint32_t rgb) {
    int x0 = x, y0 = y, x1 = x + g->w, y1 = y + g->h;
    if (x0 < t->x0) x0 = t->x0;
    if (y0 < t->y0) y0 = t->y0;
    if (x1 > t->x1) x1 = t->x1;
    if (y1 > t->y1) y1 = t->y1;
    if (x0 >= x1 || y0 >= y1) return;

    int fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
    for (int py = y0; py < y1; py++) {
        const unsigned char *src = g->bits + (size_t)(py - y) * g->w + (x0 - x);
        uint32_t *dst = t->px + (size_t)py * t->stride + x0;
        for (int px = x0; px < x1; px++, src++, dst++) {
            int a = *src;
            if (!a) continue;
            if (a == 255) { *dst = rgb; continue; }
            uint32_t d = *dst;
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dr += ((fr - dr) * a + 127) / 255;
            dg += ((fg - dg) * a + 127) / 255;
            db += ((fb - db) * a + 127) / 255;
            *dst = (uint32_t)dr << 16 | (uint32_t)dg << 8 | (uint32_t)db;
        }
    }
}

void txt_draw(const txt_target *t, const txt_style *st, int x, int baseline,
              uint32_t rgb, const char *s, size_t len) {
    if (!len || !t || !t->px || !txt_boot()) return;

    /* Nothing of the line can show: do not even shape it. An em above the
     * baseline and half of one below covers every face on the disk. */
    int em = (st->size + 63) >> 6;
    if (baseline - em * 2 >= t->y1 || baseline + em <= t->y0) return;

    const txt_run *r = txt_shape(st, s, len);
    const txt_chain *c = txt_chain_for(st);
    rgb &= 0xFFFFFF;
    int pen = x * 64;
    for (int i = 0; i < r->n; i++) {
        const txt_g *g = &r->g[i];
        const txt_slot *sl = &c->s[g->slot];
        txt_face *f = txt_face_get(sl->face);
        if (!f) continue;
        int adv = txt_scale(g->adv, st->size, f->upem);
        int gx = pen + txt_scale(g->dx, st->size, f->upem);
        int gy = txt_scale(g->dy, st->size, f->upem);
        pen += adv;
        if ((gx >> 6) - em >= t->x1) break;          /* the rest is off to the right */
        if ((gx >> 6) + em * 2 < t->x0) continue;    /* this one is off to the left  */

        const glyph_t *gl = glyph(sl->face, g->gid, st->size, (gx & 63) >> 4,
                                  sl->bold, sl->italic);
        if (!gl || !gl->bits) continue;
        blit(t, gl, (gx >> 6) + gl->left, baseline - ((gy + 32) >> 6) - gl->top, rgb);
    }
}
