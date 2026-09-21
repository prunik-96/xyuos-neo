#ifndef GFX_FONT_H
#define GFX_FONT_H

#include <stdint.h>

/* The anti-aliased TrueType renderer, and the set of characters it can draw.
 *
 * Glyphs are cached in a flat table of fixed-size cells -- one rasterisation
 * at boot, then a blend per pixel. The table is indexed by SLOT, not by
 * character: the alphabets worth carrying are not contiguous, so a small
 * mapping turns a Unicode code point into a slot and everything else asks for
 * that. Userland gets a copy of the whole table through SYS_FONT and uses the
 * same mapping, so a program draws with the same face as the window manager
 * instead of shipping a two-megabyte font of its own. */

/* 32..126       ASCII                          -> slots  32..126
 * U+00A0..00FF  Latin-1: accents, << >>, deg    -> slots 128..223
 * U+0100..017F  Latin Extended-A: cz, pl, tr    -> slots 224..351
 * U+0400..045F  Cyrillic, including Yo          -> slots 352..447    */
#define FONT_SLOTS 448

/* The slot holding `cp`, or -1 if the face does not carry it. */
int      font_slot(int cp);

/* One UTF-8 sequence -> a code point. Returns the bytes consumed, always at
 * least one, so a malformed byte cannot stall a loop. */
int      utf8_decode(const char *s, int len, int *cp);

/* Initialize at the given pixel height. Returns 1 on success. */
int      font_init(uint32_t pixel_height);
int      font_ready(void);

/* Monospace cell dimensions (pixels) chosen at init. */
uint32_t font_cell_w(void);
uint32_t font_cell_h(void);

/* Draw code point `cp` at (px,py), blending its coverage between bg and fg
 * (both 0x00RRGGBB). Fills the whole cell, background first. */
void     font_draw_glyph(uint32_t px, uint32_t py, int cp, uint32_t fg, uint32_t bg);

/* Transparent variant: blends over whatever is already there, for text on a
 * gradient or the wallpaper. */
void     font_draw_glyph_t(uint32_t px, uint32_t py, int cp, uint32_t fg);

/* The cached coverage bitmap for `cp` (cell_w * cell_h bytes), or NULL. */
const uint8_t *font_glyph(int cp);

/* The same, by slot -- what SYS_FONT copies out. */
const uint8_t *font_glyph_slot(int slot);

#endif
