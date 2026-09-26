/* libtext: a string turned into pixels, the way a browser needs it done.
 *
 * Four libraries do the hard parts, and this is what makes them one thing a
 * program can call:
 *
 *   SheenBidi    which way each stretch of the string runs      (UAX #9)
 *   HarfBuzz     which glyphs, and where: kerning, ligatures, marks placed
 *                over their letters, Arabic letters joined, Devanagari
 *                reordered
 *   FreeType     each glyph's outline turned into anti-aliased pixels
 *   libunibreak  where a line may break (UAX #14), where a character ends
 *                (UAX #29)
 *
 * The part that is ours is choosing a font for every character from the ones
 * on the disk, so that one string mixing Latin, Arabic and Chinese draws all
 * three instead of boxes. See txt_font.c for the order they are tried in.
 *
 * Measuring and drawing both start from the same shaping of the string, so
 * what is measured is exactly what is drawn -- in a browser, the difference
 * between a line that fits and a line whose last word is cut in half.
 *
 * One thread of a program uses it at a time: nothing in here is locked.
 */
#ifndef XYUOS_TEXT_H
#define XYUOS_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { TXT_SANS = 0, TXT_SERIF = 1, TXT_MONO = 2 } txt_family;

typedef struct {
    txt_family family;
    int weight;        /* 100..900 as in CSS; 600 and up is drawn bold      */
    int italic;        /* nonzero for italic or oblique                     */
    int size;          /* the em, in 64ths of a pixel: 16px is 1024         */
} txt_style;

/* Find the fonts. `dir` is where they are, NULL meaning /fonts. Returns how
 * many of the expected files were there; 0 means the caller should fall back
 * to whatever it drew with before. Safe to call more than once. */
int txt_init(const char *dir);

/* How far the face reaches above and below the baseline, in whole pixels,
 * both as positive numbers. */
void txt_metrics(const txt_style *st, int *ascent, int *descent);

/* The width of the string in pixels, exactly as txt_draw will lay it out. */
int txt_width(const txt_style *st, const char *s, size_t len);

/* The character boundary nearest to x pixels from the start of the string:
 * its byte offset, and in *at its distance from the start. Never lands in
 * the middle of a character -- e plus a combining accent is one. */
size_t txt_hit(const txt_style *st, const char *s, size_t len, int x, int *at);

/* Where to end a line that has `width` pixels: the last place the line may
 * break (UAX #14) at which the part before it still fits. Returns the byte
 * offset where the next line starts, or where the space that separates the
 * two lines is -- that space belongs to neither; *at is the width of what
 * stays on this line.
 *
 * When not even the first piece fits, the answer is the end of that first
 * piece: a word too long for the line overflows rather than vanishing. When
 * the string cannot be broken anywhere, the answer is `len`. Never 0. */
size_t txt_split(const txt_style *st, const char *s, size_t len, int width,
                 int *at);

/* Somewhere to draw: 32-bit 0x00RRGGBB pixels, `stride` of them per row,
 * and a clip rectangle -- only [x0, x1) x [y0, y1) is ever written. */
typedef struct {
    uint32_t *px;
    int stride;
    int x0, y0, x1, y1;
} txt_target;

/* Draw the string with its baseline at `baseline` and starting at `x`. */
void txt_draw(const txt_target *t, const txt_style *st, int x, int baseline,
              uint32_t rgb, const char *s, size_t len);

/* --- for tests: what the shaping produced -------------------------------- */

typedef struct {
    int face;          /* which font: see txt_face_name                     */
    unsigned gid;      /* the glyph within it                               */
    unsigned cluster;  /* byte offset of the text it came from              */
    int x_advance;     /* 64ths of a pixel, at the style's size             */
    int x_offset, y_offset;
} txt_glyph;

/* The glyphs in the order they are drawn, left to right. Returns how many
 * there were, which may be more than `max` -- only `max` are written. */
int txt_shape_info(const txt_style *st, const char *s, size_t len,
                   txt_glyph *out, int max);

/* The font file behind a face number, e.g. "NotoSansArabic-Regular.ttf". */
const char *txt_face_name(int face);

/* The glyph's name as the font records it ("f_i", "uni0628.init"), or ""
 * when the font keeps no names. Good until the next call. */
const char *txt_glyph_name(int face, unsigned gid);

/* How many font files have been read into memory so far. */
int txt_faces_loaded(void);

#ifdef __cplusplus
}
#endif

#endif
