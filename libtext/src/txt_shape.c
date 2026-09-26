/* From a string to glyphs, left to right.
 *
 * Three passes, each one a question the next one depends on:
 *
 * 1. Which way does each stretch run? A string with no right-to-left
 *    character in it is one left-to-right run and the question is skipped.
 *    Otherwise SheenBidi answers it (UAX #9), and hands back the runs already
 *    in the order they are seen, left to right.
 *
 * 2. Within a run, which font, and which script? Each character gets the
 *    first font in the style's chain that has it (txt_font.c), except where
 *    it must not be separated from what came before: a combining accent, a
 *    vowel sign, a joiner, a variation selector stay with their letter even
 *    if some other font has them too. Punctuation and digits, which belong to
 *    every script, stay in the font they follow when that font has them.
 *    The run is cut wherever the font or the script changes.
 *
 * 3. Each piece goes to HarfBuzz, which decides the glyphs and where they go
 *    -- with the whole string as context, so Arabic letters either side of a
 *    font change still know their neighbours. A right-to-left run's pieces
 *    are laid down last first: HarfBuzz reverses the glyphs inside a piece,
 *    the order of the pieces is ours to reverse.
 *
 * Positions come out in each font's own units, which is what makes one
 * shaping good at every size: a page measures its text at one size and
 * draws it at the same, but a zoom need not shape anything again.
 */
#include <stdlib.h>
#include <string.h>

#include <SheenBidi/SheenBidi.h>

#include "txt_internal.h"

static hb_buffer_t *hbuf;

/* Scratch, grown as needed and never shrunk: one entry per character. */
static uint32_t *cps;           /* the characters                          */
static unsigned *offs;          /* where each starts, in bytes             */
static short    *slots;         /* which place in the chain draws it       */
static int      *scripts;       /* hb_script_t, Common resolved away       */
static size_t    cap;

/* The glyphs of the string being shaped. */
static txt_g *out;
static int    out_n, out_cap;

static int grow(size_t n) {
    if (n <= cap) return 1;
    size_t c = cap ? cap : 256;
    while (c < n) c *= 2;
    uint32_t *a = realloc(cps, c * sizeof *a);     if (a) cps = a;
    unsigned *b = realloc(offs, c * sizeof *b);    if (b) offs = b;
    short    *d = realloc(slots, c * sizeof *d);   if (d) slots = d;
    int      *e = realloc(scripts, c * sizeof *e); if (e) scripts = e;
    if (!a || !b || !d || !e) return 0;
    cap = c;
    return 1;
}

static int emit(const txt_g *g) {
    if (out_n == out_cap) {
        int c = out_cap ? out_cap * 2 : 256;
        txt_g *n = realloc(out, (size_t)c * sizeof *n);
        if (!n) return 0;
        out = n;
        out_cap = c;
    }
    out[out_n++] = *g;
    return 1;
}

/* One character. Anything that is not well-formed UTF-8 is one byte of
 * U+FFFD, which is also what HarfBuzz makes of it. */
static uint32_t decode(const unsigned char *s, size_t len, size_t *i) {
    unsigned c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int need;
    uint32_t v;
    if      ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else { (*i)++; return 0xFFFD; }
    for (int k = 1; k <= need; k++) {
        if (*i + k >= len || (s[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        v = (v << 6) | (s[*i + k] & 0x3F);
    }
    *i += (size_t)need + 1;
    return v;
}

/* Must this character be drawn by the same font as the one before it? */
static int clings(uint32_t cp) {
    switch (hb_unicode_general_category(txt_ucd, cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:      /* joiners, bidi marks */
        return 1;
    default:
        break;
    }
    return (cp >= 0x1F3FB && cp <= 0x1F3FF)       /* skin tone          */
        || (cp >= 0xE0020 && cp <= 0xE007F);      /* emoji tag sequence */
}

static int neutral(int sc) {
    return sc == HB_SCRIPT_COMMON || sc == HB_SCRIPT_INHERITED ||
           sc == HB_SCRIPT_UNKNOWN;
}

/* Is anything in the string written right to left? The common case is no,
 * and then there is nothing to reorder and no reason to ask SheenBidi. */
static int any_rtl(size_t n) {
    for (size_t k = 0; k < n; k++) {
        uint32_t c = cps[k];
        if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
            (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
            (c >= 0x1E800 && c <= 0x1EFFF) ||
            c == 0x200F || c == 0x202B || c == 0x202E || c == 0x2067)
            return 1;
    }
    return 0;
}

/* The character that starts at byte `b`. */
static size_t index_of(size_t n, unsigned b) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (offs[mid] < b) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void shape_piece(const txt_chain *c, const char *s, size_t len,
                        size_t k0, size_t k1, size_t n, int rtl) {
    int slot = slots[k0];
    txt_face *f = txt_face_get(c->s[slot].face);
    if (!f) return;
    unsigned b0 = offs[k0], b1 = k1 < n ? offs[k1] : (unsigned)len;

    hb_buffer_clear_contents(hbuf);
    hb_buffer_add_utf8(hbuf, s, (int)len, b0, (int)(b1 - b0));
    hb_buffer_set_direction(hbuf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(hbuf, (hb_script_t)scripts[k0]);
    hb_buffer_guess_segment_properties(hbuf);
    hb_shape(f->hb, hbuf, NULL, 0);

    unsigned gn = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(hbuf, &gn);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(hbuf, &gn);
    for (unsigned j = 0; j < gn; j++) {
        txt_g g = { (short)slot, info[j].codepoint, info[j].cluster,
                    pos[j].x_advance, pos[j].x_offset, pos[j].y_offset };
        if (!emit(&g)) return;
    }
}

/* One run of one direction, characters [k0, k1). */
static void shape_run(const txt_chain *c, const char *s, size_t len,
                      size_t k0, size_t k1, size_t n, int rtl) {
    if (k0 >= k1) return;

    /* Fonts. */
    int prev = -1;
    for (size_t k = k0; k < k1; k++) {
        uint32_t cp = cps[k];
        int sc = hb_unicode_script(txt_ucd, cp);
        int slot;
        if (prev >= 0 && clings(cp))
            slot = prev;
        else if (prev >= 0 && neutral(sc) && txt_has(c->s[prev].face, cp))
            slot = prev;
        else {
            slot = txt_pick(c, cp);
            if (slot < 0) slot = prev >= 0 ? prev : 0;   /* the box it is */
        }
        slots[k] = (short)slot;
        prev = slot;
        scripts[k] = neutral(sc) ? -1 : sc;
    }

    /* Scripts: a neutral character takes the one before it, and the neutral
     * ones at the start take the first real one after them. */
    int last = -1;
    for (size_t k = k0; k < k1; k++) {
        if (scripts[k] >= 0) last = scripts[k];
        else scripts[k] = last;
    }
    int first = HB_SCRIPT_COMMON;
    for (size_t k = k0; k < k1; k++)
        if (scripts[k] >= 0) { first = scripts[k]; break; }
    for (size_t k = k0; k < k1 && scripts[k] < 0; k++) scripts[k] = first;

    /* Pieces, in reading order; laid down in seeing order. */
    size_t starts[64];
    size_t np = 0;
    size_t k = k0;
    while (k < k1) {
        size_t e = k + 1;
        while (e < k1 && slots[e] == slots[k] && scripts[e] == scripts[k]) e++;
        if (np == 64) {
            /* Too many to reverse in one go: flush what there is. Only a run
             * of sixty-four font changes gets here, and then a right-to-left
             * one is drawn in two halves in the wrong order -- still every
             * glyph, which beats dropping any. */
            for (size_t p = 0; p < np; p++) {
                size_t q = rtl ? np - 1 - p : p;
                size_t end = q + 1 < np ? starts[q + 1] : k;
                shape_piece(c, s, len, starts[q], end, n, rtl);
            }
            np = 0;
        }
        starts[np++] = k;
        k = e;
    }
    for (size_t p = 0; p < np; p++) {
        size_t q = rtl ? np - 1 - p : p;
        size_t end = q + 1 < np ? starts[q + 1] : k1;
        shape_piece(c, s, len, starts[q], end, n, rtl);
    }
}

static void shape_all(const txt_chain *c, const char *s, size_t len) {
    out_n = 0;
    if (!hbuf) hbuf = hb_buffer_create();
    if (!grow(len + 1)) return;

    size_t n = 0;
    for (size_t i = 0; i < len; ) {
        offs[n] = (unsigned)i;
        cps[n] = decode((const unsigned char *)s, len, &i);
        n++;
    }
    if (n == 0) return;

    if (!any_rtl(n)) {
        shape_run(c, s, len, 0, n, n, 0);
        return;
    }

    SBCodepointSequence seq = { SBStringEncodingUTF8, s, len };
    SBAlgorithmRef algo = SBAlgorithmCreate(&seq);
    if (!algo) { shape_run(c, s, len, 0, n, n, 0); return; }
    SBUInteger at = 0;
    while (at < len) {
        SBParagraphRef para = SBAlgorithmCreateParagraph(algo, at, len - at,
                                                         SBLevelDefaultLTR);
        if (!para) break;
        SBUInteger plen = SBParagraphGetLength(para);
        SBLineRef line = SBParagraphCreateLine(para, at, plen);
        if (line) {
            SBUInteger rn = SBLineGetRunCount(line);
            const SBRun *runs = SBLineGetRunsPtr(line);
            for (SBUInteger r = 0; r < rn; r++) {
                size_t k0 = index_of(n, (unsigned)runs[r].offset);
                size_t k1 = index_of(n, (unsigned)(runs[r].offset + runs[r].length));
                shape_run(c, s, len, k0, k1, n, runs[r].level & 1);
            }
            SBLineRelease(line);
        }
        SBParagraphRelease(para);
        if (plen == 0) break;
        at += plen;
    }
    SBAlgorithmRelease(algo);
}

/* --- the cache ------------------------------------------------------------ */

/* A page is drawn again every time it scrolls, and every string on it is the
 * same string it was last time. Shaping is the expensive step, so the result
 * is kept, keyed by the style's chain and the bytes themselves. */
#define RUN_CACHE 1024
static txt_run cache[RUN_CACHE];
static txt_run spare;           /* when there is no memory to keep one */

static uint32_t hash(const char *s, size_t len, int chain) {
    uint32_t h = 2166136261u ^ (uint32_t)chain;
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

const txt_run *txt_shape(const txt_style *st, const char *s, size_t len) {
    const txt_chain *c = txt_chain_for(st);
    uint32_t h = hash(s, len, c->id);
    txt_run *r = &cache[h & (RUN_CACHE - 1)];
    if (r->text && r->chain == c->id && r->hash == h && r->len == len &&
        memcmp(r->text, s, len) == 0)
        return r;

    shape_all(c, s, len);

    char *t = malloc(len ? len : 1);
    txt_g *g = malloc((size_t)(out_n ? out_n : 1) * sizeof *g);
    if (!t || !g) {
        free(t);
        free(g);
        spare.chain = c->id;
        spare.n = out_n;
        spare.g = out;
        spare.len = len;
        return &spare;
    }
    free(r->text);
    free(r->g);
    memcpy(t, s, len);
    memcpy(g, out, (size_t)out_n * sizeof *g);
    r->chain = c->id;
    r->hash = h;
    r->len = len;
    r->text = t;
    r->n = out_n;
    r->g = g;
    return r;
}

int txt_adv(const txt_style *st, const txt_chain *c, const txt_g *g) {
    txt_face *f = txt_face_get(c->s[g->slot].face);
    return f ? txt_scale(g->adv, st->size, f->upem) : 0;
}
