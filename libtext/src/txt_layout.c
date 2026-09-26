/* Measuring: how wide, where did the click land, where does the line end.
 *
 * All three come from the one shaping txt_draw uses, never from adding up
 * the widths of characters one at a time. Those differ: kerning pulls "AV"
 * together, a ligature makes "fi" one glyph, an Arabic letter is a different
 * width at the start of a word than at the end. A line measured one way and
 * drawn the other either overflows or leaves a gap.
 *
 * The positions inside a string are in reading order: the width of the text
 * before byte b is the sum of the glyphs HarfBuzz made from the bytes before
 * b. For a string that mixes directions that is the width the part before b
 * would have, which is the question a line breaker is asking.
 */
#include <stdlib.h>
#include <string.h>

#include "txt_internal.h"
#include "linebreak.h"
#include "graphemebreak.h"

static int   *pre;              /* pre[b]: width before byte b, 26.6       */
static char  *brk;              /* libunibreak's answer for each byte      */
static size_t pcap;

static int room(size_t len) {
    if (len + 1 <= pcap) return 1;
    size_t c = pcap ? pcap : 256;
    while (c < len + 1) c *= 2;
    int *p = realloc(pre, c * sizeof *p);
    if (p) pre = p;
    char *b = realloc(brk, c);
    if (b) brk = b;
    if (!p || !b) return 0;
    pcap = c;
    return 1;
}

/* Fill pre[0..len]; returns the whole width, 26.6. */
static int prefix(const txt_style *st, const txt_run *r, size_t len) {
    const txt_chain *c = txt_chain_for(st);
    memset(pre, 0, (len + 1) * sizeof *pre);
    int total = 0;
    for (int i = 0; i < r->n; i++) {
        int a = txt_adv(st, c, &r->g[i]);
        total += a;
        size_t at = r->g[i].cluster < len ? r->g[i].cluster : len - 1;
        pre[at + 1] += a;
    }
    for (size_t b = 1; b <= len; b++) pre[b] += pre[b - 1];
    return total;
}

static int px(int v) { return (v + 32) >> 6; }

int txt_width(const txt_style *st, const char *s, size_t len) {
    if (!len || !txt_boot()) return 0;
    const txt_run *r = txt_shape(st, s, len);
    const txt_chain *c = txt_chain_for(st);
    int total = 0;
    for (int i = 0; i < r->n; i++) total += txt_adv(st, c, &r->g[i]);
    return px(total);
}

size_t txt_hit(const txt_style *st, const char *s, size_t len, int x, int *at) {
    if (!len || x <= 0 || !room(len) || !txt_boot()) { if (at) *at = 0; return 0; }
    const txt_run *r = txt_shape(st, s, len);
    prefix(st, r, len);
    set_graphemebreaks_utf8((const utf8_t *)s, len, NULL, brk);

    /* The boundaries are 0, len, and wherever one character ends and the
     * next begins; take the one nearest to x. */
    int want = x * 64;
    size_t best = 0;
    int dist = want;
    for (size_t i = 0; i < len; i++) {
        if (i + 1 < len && brk[i] != GRAPHEMEBREAK_BREAK) continue;
        size_t b = i + 1;
        int d = pre[b] > want ? pre[b] - want : want - pre[b];
        if (d < dist) { dist = d; best = b; }
    }
    if (at) *at = px(pre[best]);
    return best;
}

size_t txt_split(const txt_style *st, const char *s, size_t len, int width,
                 int *at) {
    if (!len || !room(len) || !txt_boot()) { if (at) *at = 0; return len; }
    const txt_run *r = txt_shape(st, s, len);
    int total = prefix(st, r, len);
    set_linebreaks_utf8((const utf8_t *)s, len, NULL, brk);

    /* A break "after byte i" means the next line starts at i + 1. What stays
     * on this line is everything before that, less the spaces it ends with:
     * they are not drawn at the end of a line, so they do not count. */
    int limit = width * 64;
    size_t best = 0, best_end = 0, first = 0, first_end = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (brk[i] != LINEBREAK_ALLOWBREAK && brk[i] != LINEBREAK_MUSTBREAK)
            continue;
        size_t b = i + 1, e = b;
        while (e > 0 && s[e - 1] == ' ') e--;
        if (e == 0) continue;               /* nothing would stay behind */
        if (!first) { first = b; first_end = e; }
        if (pre[e] > limit) break;
        best = b;
        best_end = e;
    }
    if (!best) { best = first; best_end = first_end; }
    if (!best) {
        if (at) *at = px(total);
        return len;
    }
    if (at) *at = px(pre[best_end]);
    /* The caller drops a space at the split point and draws nothing for it,
     * so the answer points at the space when there is one. */
    return best_end < best ? best_end : best;
}

int txt_shape_info(const txt_style *st, const char *s, size_t len,
                   txt_glyph *out, int max) {
    if (!len || !txt_boot()) return 0;
    const txt_run *r = txt_shape(st, s, len);
    const txt_chain *c = txt_chain_for(st);
    for (int i = 0; i < r->n && i < max; i++) {
        const txt_g *g = &r->g[i];
        txt_face *f = txt_face_get(c->s[g->slot].face);
        int upem = f ? f->upem : 1000;
        out[i].face = c->s[g->slot].face;
        out[i].gid = g->gid;
        out[i].cluster = g->cluster;
        out[i].x_advance = txt_scale(g->adv, st->size, upem);
        out[i].x_offset = txt_scale(g->dx, st->size, upem);
        out[i].y_offset = txt_scale(g->dy, st->size, upem);
    }
    return r->n;
}
