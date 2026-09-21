#include "font.h"
#include "kmath.h"
#include "stb_truetype.h"
#include "font_ttf.h"          // font_ttf[], font_ttf_len
#include "../mm/heap.h"
#include "../drivers/framebuffer.h"

/* The three stretches of Unicode worth carrying, and where each lands in the
 * flat table. Kept in one place because userland's copy in gui.h has to agree
 * with it exactly -- a mismatch shows up as the wrong letters, not as an
 * error. */
static const struct { int first, last, slot; } RANGES[] = {
    {   0x20,  0x7E,  0x20 },   /* ASCII                                  */
    {   0xA0,  0xFF,  128  },   /* Latin-1: accented letters, << >>, deg  */
    {  0x100, 0x17F,  224  },   /* Latin Extended-A: Czech, Polish, ...   */
    {  0x400, 0x45F,  352  },   /* Cyrillic, Yo included                  */
};

int font_slot(int cp) {
    for (unsigned i = 0; i < sizeof RANGES / sizeof RANGES[0]; i++)
        if (cp >= RANGES[i].first && cp <= RANGES[i].last)
            return RANGES[i].slot + (cp - RANGES[i].first);
    return -1;
}

int utf8_decode(const char *s, int len, int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }

    int need, v;
    if      ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }

    if (len < need + 1) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i <= need; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (k & 0x3F);
    }
    *cp = v;
    return need + 1;
}

static stbtt_fontinfo font;
static int    ready = 0;
static float  scale = 0.0f;
static int    ascent_px = 0;
static uint32_t cell_w = 0, cell_h = 0;
static uint8_t *glyphs[FONT_SLOTS];   // coverage bitmaps, cell_w*cell_h, or NULL

static int iround(float x) {
    return (int)k_floor((double)x + 0.5);
}

int font_init(uint32_t pixel_height) {
    if (!stbtt_InitFont(&font, font_ttf, stbtt_GetFontOffsetForIndex(font_ttf, 0)))
        return 0;

    scale = stbtt_ScaleForPixelHeight(&font, (float)pixel_height);

    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    ascent_px = iround(asc * scale);
    int descent_px = iround(desc * scale);
    cell_h = (uint32_t)(ascent_px - descent_px);
    if (cell_h < pixel_height) cell_h = pixel_height;

    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font, 'M', &adv, &lsb);
    cell_w = (uint32_t)iround(adv * scale);
    if (cell_w < 2) cell_w = 2;

    for (int i = 0; i < FONT_SLOTS; i++) glyphs[i] = 0;

    for (unsigned r = 0; r < sizeof RANGES / sizeof RANGES[0]; r++)
    for (int ch = RANGES[r].first; ch <= RANGES[r].last; ch++) {
        int slot = RANGES[r].slot + (ch - RANGES[r].first);

        /* A face that does not have the character at all gets no cell, and
         * font_glyph returns NULL for it -- the caller draws nothing rather
         * than a box of noise. */
        if (!stbtt_FindGlyphIndex(&font, ch)) continue;

        uint8_t *cell = (uint8_t *)kmalloc(cell_w * cell_h);
        if (!cell) return 0;
        for (uint32_t k = 0; k < cell_w * cell_h; k++) cell[k] = 0;
        glyphs[slot] = cell;

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font, ch, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;   // space / empty glyph

        uint8_t *tmp = (uint8_t *)kmalloc((uint32_t)(gw * gh));
        if (!tmp) continue;
        stbtt_MakeCodepointBitmap(&font, tmp, gw, gh, gw, scale, scale, ch);

        int dstx0 = x0;             // left side bearing offset
        int dsty0 = ascent_px + y0; // y0 is negative above baseline
        for (int ty = 0; ty < gh; ty++) {
            int dy = dsty0 + ty;
            if (dy < 0 || dy >= (int)cell_h) continue;
            for (int tx = 0; tx < gw; tx++) {
                int dx = dstx0 + tx;
                if (dx < 0 || dx >= (int)cell_w) continue;
                cell[dy * cell_w + dx] = tmp[ty * gw + tx];
            }
        }
        kfree(tmp);
    }

    ready = 1;
    return 1;
}

int font_ready(void)       { return ready; }

const uint8_t *font_glyph(int cp) {
    if (!ready) return 0;
    int slot = font_slot(cp);
    return (slot >= 0) ? glyphs[slot] : 0;
}

const uint8_t *font_glyph_slot(int slot) {
    if (!ready || slot < 0 || slot >= FONT_SLOTS) return 0;
    return glyphs[slot];
}
uint32_t font_cell_w(void) { return cell_w; }
uint32_t font_cell_h(void) { return cell_h; }

static inline uint32_t blend(uint32_t bg, uint32_t fg, uint8_t cov) {
    uint32_t br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;
    uint32_t ic = 255 - cov;
    uint32_t r = (br * ic + fr * cov) / 255;
    uint32_t g = (bgc * ic + fgc * cov) / 255;
    uint32_t b = (bb * ic + fb * cov) / 255;
    return (r << 16) | (g << 8) | b;
}

void font_draw_glyph(uint32_t px, uint32_t py, int cp, uint32_t fg, uint32_t bg) {
    if (!ready) return;
    uint8_t *base = (uint8_t *)fb_get_base();
    if (!base) return;
    fb_mark_rect(px, py, cell_w, cell_h);
    uint32_t pitch = fb_get_pitch();
    uint32_t fw = fb_get_width(), fh = fb_get_height();

    const uint8_t *g = font_glyph(cp);

    for (uint32_t cy = 0; cy < cell_h; cy++) {
        uint32_t y = py + cy;
        if (y >= fh) break;
        uint32_t *row = (uint32_t *)(base + y * pitch);
        for (uint32_t cx = 0; cx < cell_w; cx++) {
            uint32_t x = px + cx;
            if (x >= fw) break;
            uint8_t cov = g ? g[cy * cell_w + cx] : 0;
            row[x] = cov ? blend(bg, fg, cov) : bg;
        }
    }
}

// Like font_draw_glyph but transparent: blends the glyph over whatever is
// already in the framebuffer, so text sits cleanly on a gradient/wallpaper.
void font_draw_glyph_t(uint32_t px, uint32_t py, int cp, uint32_t fg) {
    if (!ready) return;
    uint8_t *base = (uint8_t *)fb_get_base();
    if (!base) return;
    fb_mark_rect(px, py, cell_w, cell_h);
    uint32_t pitch = fb_get_pitch();
    uint32_t fw = fb_get_width(), fh = fb_get_height();
    const uint8_t *g = font_glyph(cp);
    for (uint32_t cy = 0; cy < cell_h; cy++) {
        uint32_t y = py + cy;
        if (y >= fh) break;
        uint32_t *row = (uint32_t *)(base + y * pitch);
        for (uint32_t cx = 0; cx < cell_w; cx++) {
            uint32_t x = px + cx;
            if (x >= fw) break;
            uint8_t cov = g ? g[cy * cell_w + cx] : 0;
            if (cov) row[x] = blend(row[x], fg, cov);
        }
    }
}
