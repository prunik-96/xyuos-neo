/* What the three parts of libtext share. Nothing here is public. */
#ifndef TXT_INTERNAL_H
#define TXT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include "text.h"

/* --- fonts (txt_font.c) --------------------------------------------------- */

/* One font file. Read into memory the first time a character needs it, and
 * then read by FreeType and HarfBuzz from the same bytes: FreeType for the
 * outlines, HarfBuzz for everything that decides which outlines and where. */
typedef struct {
    const char *file;
    int         state;          /* 0 not tried, 1 ready, -1 missing or bad  */
    unsigned char *data;
    size_t      size;
    FT_Face     ft;
    hb_font_t  *hb;             /* at the font's own scale: units per em    */
    int         upem;
    int         ft_size;        /* the size the FT_Face is set to, 26.6     */
} txt_face;

/* A place in a style's fallback order: which face, and what it has to fake
 * because the file does not have it -- a bold weight, an italic slant. */
typedef struct {
    short face;
    char  bold, italic;
} txt_slot;

#define TXT_SLOTS_MAX 48

typedef struct {
    int      id;                /* family * 4 + bold * 2 + italic           */
    int      n;
    txt_slot s[TXT_SLOTS_MAX];
} txt_chain;

extern FT_Library txt_ftlib;
extern hb_unicode_funcs_t *txt_ucd;

/* Start FreeType and the tables, once. 0 if that failed and nothing can be
 * shaped or drawn. Every public entry point calls it; it costs one test. */
int txt_boot(void);

/* The face, loaded if it has not been. NULL if it cannot be. */
txt_face *txt_face_get(int i);

/* The fallback order for a style. */
const txt_chain *txt_chain_for(const txt_style *st);

/* The first place in the chain whose font has this character, or -1 if
 * none has it. */
int txt_pick(const txt_chain *c, uint32_t cp);

/* Does this face have the character? */
int txt_has(int face, uint32_t cp);

/* --- shaping (txt_shape.c) ------------------------------------------------ */

/* One glyph as HarfBuzz placed it, in the units of its own font: a shaping is
 * the same at every size, so one is kept per string and scaled on use. */
typedef struct {
    short    slot;              /* where in the chain its font is           */
    unsigned gid;
    unsigned cluster;           /* byte offset of the text it stands for    */
    int      adv, dx, dy;       /* font units                               */
} txt_g;

typedef struct {
    int       chain;            /* txt_chain.id it was shaped with          */
    uint32_t  hash;
    size_t    len;
    char     *text;             /* a copy, to tell one string from another  */
    int       n;
    txt_g    *g;                /* left to right, as drawn                  */
} txt_run;

/* The string shaped with the style's fallback order. Kept in a cache; the
 * pointer is good until the next call. */
const txt_run *txt_shape(const txt_style *st, const char *s, size_t len);

/* A font-unit length at this size, in 64ths of a pixel. */
static inline int txt_scale(int v, int size, int upem) {
    int64_t p = (int64_t)v * size;
    return (int)(p >= 0 ? (p + upem / 2) / upem : -((-p + upem / 2) / upem));
}

/* The advance of glyph i, in 64ths of a pixel. */
int txt_adv(const txt_style *st, const txt_chain *c, const txt_g *g);

#endif
