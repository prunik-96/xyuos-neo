#ifndef GFX_UIFONT_H
#define GFX_UIFONT_H

#include <stdint.h>

/* The face the desktop speaks in: Noto Sans, proportional, in a handful of
 * sizes. The terminal keeps its monospaced cells (font.h); everything around
 * it -- titles, the taskbar, menus -- uses this.
 *
 * Every glyph of every face is rasterised once, at start-up, by stb_truetype
 * with the floating-point unit on. After that drawing text is copying bytes:
 * the window manager paints inside system calls, where the floating-point
 * registers belong to the program that made the call, so nothing at draw time
 * may touch them. */

enum {
    UI_F11,        /* 11 px: the date under the clock, small print       */
    UI_F12,        /* 12 px: lists, labels                                */
    UI_F13,        /* 13 px: window titles, menus -- the body text       */
    UI_F13B,
    UI_F15,        /* 15 px: headings inside menus                        */
    UI_F15B,
    UI_F20B,       /* 20 px bold: big headings                            */
    UI_F28,        /* 28 px: the clock on a tile, the boot screen        */
    UI_F44,        /* 44 px: the big clock                                */
    UI_FACES
};

/* Where a code point lives in the glyph tables: the four alphabets font.h
 * carries, in the same slots, then a short list of punctuation and symbols
 * an interface needs and a terminal does not. */
#define UI_BASE_SLOTS 448
static const uint16_t UI_EXTRA[] = {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
    0x2116, 0x20AC, 0x20BD, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212,
    0x2713, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x2630, 0x2715,
};
#define UI_NEXTRA (int)(sizeof UI_EXTRA / sizeof UI_EXTRA[0])
#define UI_SLOTS  (UI_BASE_SLOTS + UI_NEXTRA)

static inline int ui_slot(int cp) {
    if (cp >= 0x20 && cp <= 0x7E)   return cp;
    if (cp >= 0xA0 && cp <= 0xFF)   return 128 + (cp - 0xA0);
    if (cp >= 0x100 && cp <= 0x17F) return 224 + (cp - 0x100);
    if (cp >= 0x400 && cp <= 0x45F) return 352 + (cp - 0x400);
    for (int i = 0; i < UI_NEXTRA; i++) if (UI_EXTRA[i] == cp) return UI_BASE_SLOTS + i;
    return -1;
}

struct ui_glyph {
    int8_t   xoff, yoff;   /* bitmap's top-left from the pen on the baseline */
    uint8_t  w, h;         /* 0x0 for a space                                */
    uint8_t  adv;          /* how far the pen moves, whole pixels             */
    uint8_t  present;      /* the face has this character                      */
    uint32_t off;          /* into the face's pixel pool                       */
};

struct ui_face {
    int      px;           /* nominal size (em height, pixels)               */
    int      ascent;       /* baseline below the top of the line box          */
    int      height;       /* line box: ascent + descent                      */
    uint8_t *pool;         /* every glyph's coverage, 0..255                  */
    struct ui_glyph g[UI_SLOTS];
};

extern struct ui_face ui_faces[UI_FACES];
extern int ui_fonts_ready;

/* Load the two TrueType files and rasterise every face. Runs once, at
 * start-up, before the first frame. Returns 1 if the desktop has a font. */
int uifont_init(void);

#endif
