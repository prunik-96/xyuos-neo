#ifndef WM_GLYPHS_H
#define WM_GLYPHS_H

#include <stdint.h>

/* The desktop's small pictures: soft filled glyphs, drawn by librast into
 * whatever ui.c is drawing into. Integer only, like everything the window
 * manager draws. Until the drawn program icons exist, a program's icon is
 * one of these in white on a rounded square of its colour. */

enum {
    G_FOLDER, G_GLOBE, G_TERM, G_NOTE, G_MUSIC, G_GEAR, G_CHART, G_IMAGE, G_POWER,
    G_SEARCH, G_WIFI, G_VOL, G_PIN, G_MIN, G_MAX, G_CLOSE, G_PLUS, G_MOON, G_BELL,
    G_SAVE, G_PLAY, G_NEXT, G_PREV, G_DOC, G_CODE, G_GAME, G_CHIP, G_PAUSE,
    G_COUNT
};

/* Glyph g in a box s pixels square at (x, y), in `argb`; `inner` is the
 * colour of details cut into a filled shape (the lines on a page, the prompt
 * on a terminal) -- the colour of whatever the glyph sits on. */
void glyph_draw(int g, int x, int y, int s, uint32_t argb, uint32_t inner);

/* A program's icon: its glyph in white on a rounded square of its colour,
 * lit from above. */
void glyph_app(int x, int y, int s, uint32_t col, int g);

/* The xyuOS mark -- an x drawn in one flowing line -- in white on a disc of
 * `col` of radius R centred on (cx, cy). */
void glyph_logo(int cx, int cy, int R, uint32_t col);

/* What a program looks like until it has a drawn icon: its glyph and its
 * colour, looked up by the name of its file in /bin. */
void glyph_for_program(const char *name, int *g, uint32_t *col);

#endif
