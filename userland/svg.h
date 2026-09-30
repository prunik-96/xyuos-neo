#ifndef SVG_H
#define SVG_H

/* SVG rendering for xyuOS Neo.
 *
 * The other formats in img.h hand over pixels. This one hands over a drawing:
 * the markup is read here, and every shape in it is given to librast, which
 * fills and strokes it -- anti-aliased by exact area, through the transforms
 * the drawing nests, with the joins, caps and dashes it asks for.
 *
 * WHAT IS HERE: path, rect, circle, ellipse, line, polyline, polygon, and <g>
 * and nested <svg> with transforms; the whole path grammar, elliptical arcs
 * included; fill and stroke with either winding rule, stroke-linejoin,
 * -linecap, -miterlimit, -dasharray and -dashoffset; colours by name, hex,
 * rgb() and rgba(), currentColor; linear and radial gradients, with their
 * units, transforms, spread methods and href inheritance; clip-path; the
 * three opacities; viewBox scaling. The picture comes out with its
 * transparency, not only laid on a background.
 *
 * WHAT IS NOT: text (it needs a font chosen and shaped, which is libtext's
 * business and not this file's), <use>, patterns, masks, filters, and
 * <style> sheets. A shape that depends on one of those is drawn without it
 * rather than dropped -- half a logo beats a hole.
 *
 * ALL INTEGER. web.c is built with -mgeneral-regs-only, so there is no floating
 * point to be had: coordinates are 16.16 fixed point throughout, the same
 * rast_fx librast takes.
 */

#include <stdlib.h>
#include <string.h>
#include "rast.h"

/* ==========================================================================
 * numbers
 * ========================================================================== */

typedef rast_fx  svg_fx;
typedef rast_mat svg_mat;
#define FX_ONE   RAST_ONE
#define FX_HALF  (RAST_ONE / 2)

static svg_fx fx_mul(svg_fx a, svg_fx b) { return rast_mulfx(a, b); }
static svg_fx fx_div(svg_fx a, svg_fx b) { return rast_divfx(a, b); }

static int svg_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static int svg_isdigit(int c) { return c >= '0' && c <= '9'; }

static void svg_skip_sep(const char **pp) {
    const char *p = *pp;
    while (*p && (svg_isspace(*p) || *p == ',')) p++;
    *pp = p;
}

/* A number in the markup's grammar: sign, digits, fraction, exponent. Anything
 * beyond what 16.16 holds is held at its limit rather than wrapped round. */
static svg_fx svg_number(const char **pp) {
    const char *p = *pp;
    svg_skip_sep(&p);
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }

    long long ip = 0;
    while (svg_isdigit(*p)) { if (ip < 100000) ip = ip * 10 + (*p - '0'); p++; }

    long long frac = 0, fdiv = 1;
    if (*p == '.') {
        p++;
        while (svg_isdigit(*p)) {
            if (fdiv < 100000000LL) { frac = frac * 10 + (*p - '0'); fdiv *= 10; }
            p++;
        }
    }

    long long v = (ip << 16) + (fdiv > 1 ? ((frac << 16) / fdiv) : 0);

    if (*p == 'e' || *p == 'E') {
        const char *save = p;
        p++;
        int eneg = 0;
        if (*p == '+') p++;
        else if (*p == '-') { eneg = 1; p++; }
        if (svg_isdigit(*p)) {
            int ex = 0;
            while (svg_isdigit(*p)) { if (ex < 40) ex = ex * 10 + (*p - '0'); p++; }
            for (int i = 0; i < ex; i++) {
                if (eneg) v /= 10;
                else if (v < (1LL << 46)) v *= 10;
            }
        } else {
            p = save;                     /* an 'e' that began something else */
        }
    }
    if (v > 0x3FFFFFFF) v = 0x3FFFFFFF;

    *pp = p;
    return (svg_fx)(neg ? -v : v);
}

/* A flag in an arc command is a single character, with no separator required
 * after it -- "a1 1 0 011 1" is legal and means large=0 sweep=1. */
static int svg_flag(const char **pp) {
    svg_skip_sep(pp);
    int v = (**pp == '1');
    if (**pp == '0' || **pp == '1') (*pp)++;
    return v;
}

static int svg_ci_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        int x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* ==========================================================================
 * colours
 * ========================================================================== */

/* 0xAARRGGBB, with A==0 meaning "do not paint". The named colours are the
 * ones that turn up in real drawings; anything unrecognised is black, which
 * is what the specification says an invalid paint falls back to. */
static unsigned svg_color(const char *s, unsigned current) {
    while (svg_isspace(*s)) s++;
    if (!*s) return current;

    if (svg_ci_eq(s, "none", 4)) return 0;
    if (svg_ci_eq(s, "transparent", 11)) return 0;
    if (svg_ci_eq(s, "currentcolor", 12)) return current;

    if (*s == '#') {
        s++;
        unsigned v = 0; int n = 0;
        while (n < 8) {
            int c = s[n], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (unsigned)d;
            n++;
        }
        if (n == 3) return 0xFF000000u | ((v & 0xF00) * 0x1100) |
                            ((v & 0xF0) * 0x110) | ((v & 0xF) * 0x11);
        if (n == 4) return (((v & 0xF) * 0x11) << 24) | (((v >> 12) & 0xF) * 0x110000) |
                            (((v >> 8) & 0xF) * 0x1100) | (((v >> 4) & 0xF) * 0x11);
        if (n == 6) return 0xFF000000u | v;
        if (n == 8) return ((v & 0xFF) << 24) | (v >> 8);   /* #rrggbbaa */
        return 0xFF000000u;
    }

    if (svg_ci_eq(s, "rgb", 3)) {
        const char *p = s + 3;
        while (*p && *p != '(') p++;
        if (*p == '(') {
            p++;
            svg_fx c[4] = { 0, 0, 0, FX_ONE };
            for (int i = 0; i < 4 && *p && *p != ')'; i++) {
                c[i] = svg_number(&p);
                svg_skip_sep(&p);
                if (*p == '%') {
                    p++;
                    c[i] = i < 3 ? fx_mul(c[i], 255 * FX_ONE / 100) : c[i] / 100;
                }
                svg_skip_sep(&p);
                if (*p == '/') { p++; }
            }
            unsigned a = (unsigned)((c[3] > FX_ONE ? 255 : (c[3] * 255) >> 16));
            if (a > 255) a = 255;
            unsigned r = (unsigned)(c[0] >> 16), g = (unsigned)(c[1] >> 16),
                     b = (unsigned)(c[2] >> 16);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    static const struct { const char *n; int len; unsigned v; } named[] = {
        { "black",   5, 0x000000 }, { "white",   5, 0xFFFFFF },
        { "red",     3, 0xFF0000 }, { "green",   5, 0x008000 },
        { "blue",    4, 0x0000FF }, { "yellow",  6, 0xFFFF00 },
        { "gray",    4, 0x808080 }, { "grey",    4, 0x808080 },
        { "silver",  6, 0xC0C0C0 }, { "maroon",  6, 0x800000 },
        { "olive",   5, 0x808000 }, { "lime",    4, 0x00FF00 },
        { "aqua",    4, 0x00FFFF }, { "cyan",    4, 0x00FFFF },
        { "teal",    4, 0x008080 }, { "navy",    4, 0x000080 },
        { "fuchsia", 7, 0xFF00FF }, { "magenta", 7, 0xFF00FF },
        { "purple",  6, 0x800080 }, { "orange",  6, 0xFFA500 },
        { "pink",    4, 0xFFC0CB }, { "brown",   5, 0xA52A2A },
        { "gold",    4, 0xFFD700 }, { "darkgray",8, 0xA9A9A9 },
        { "darkgrey",8, 0xA9A9A9 }, { "lightgray",9,0xD3D3D3 },
        { "lightgrey",9,0xD3D3D3 }, { "dimgray", 7, 0x696969 },
        { "whitesmoke",10,0xF5F5F5 }, { "gainsboro",9,0xDCDCDC },
        { "darkred", 7, 0x8B0000 }, { "darkgreen",9,0x006400 },
        { "darkblue",8, 0x00008B }, { "lightblue",9,0xADD8E6 },
        { "skyblue", 7, 0x87CEEB }, { "steelblue",9,0x4682B4 },
        { "tomato",  6, 0xFF6347 }, { "crimson", 7, 0xDC143C },
        { "orangered",9,0xFF4500 }, { "indigo",  6, 0x4B0082 },
        { "violet",  6, 0xEE82EE }, { "coral",   5, 0xFF7F50 },
    };
    /* Longest names first would matter for prefixes; comparing whole words
     * does it instead. */
    int wl = 0;
    while (s[wl] && !svg_isspace(s[wl]) && s[wl] != ';' && s[wl] != ')') wl++;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].len == wl && svg_ci_eq(s, named[i].n, wl)) return 0xFF000000u | named[i].v;

    return 0xFF000000u;
}

/* ==========================================================================
 * attributes
 * ========================================================================== */

#define SVG_ATTR_MAX 32

typedef struct {
    const char *name; int nlen;
    const char *val;  int vlen;
} svg_attr;

/* Copy an attribute value out as a NUL-terminated string. */
static void svg_attr_str(const svg_attr *a, char *out, int max) {
    int n = a ? a->vlen : 0;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) out[i] = a->val[i];
    out[n] = 0;
}

static const svg_attr *svg_find(const svg_attr *at, int n, const char *name) {
    int len = (int)strlen(name);
    for (int i = 0; i < n; i++)
        if (at[i].nlen == len && svg_ci_eq(at[i].name, name, len)) return &at[i];
    return 0;
}

/* Read the attributes of one tag. `p` points just past the tag name; on
 * return it points at the '>' (or at the '/' of a self-closing tag). */
static int svg_attrs(const char **pp, svg_attr *at, int max) {
    const char *p = *pp;
    int n = 0;
    for (;;) {
        while (svg_isspace(*p)) p++;
        if (!*p || *p == '>' || *p == '/') break;
        const char *name = p;
        while (*p && *p != '=' && *p != '>' && *p != '/' && !svg_isspace(*p)) p++;
        int nlen = (int)(p - name);
        while (svg_isspace(*p)) p++;
        if (*p != '=') { if (nlen == 0) p++; continue; }
        p++;
        while (svg_isspace(*p)) p++;
        char q = 0;
        if (*p == '"' || *p == '\'') { q = *p; p++; }
        const char *val = p;
        if (q) { while (*p && *p != q) p++; }
        else   { while (*p && !svg_isspace(*p) && *p != '>' && *p != '/') p++; }
        int vlen = (int)(p - val);
        if (q && *p) p++;
        if (n < max) {
            at[n].name = name; at[n].nlen = nlen;
            at[n].val  = val;  at[n].vlen = vlen;
            n++;
        }
    }
    *pp = p;
    return n;
}

/* Look inside style="fill:#fff;stroke:none" for one property. Returns 1 and
 * fills `out` when found. */
static int svg_style_get(const svg_attr *style, const char *prop,
                         char *out, int max) {
    if (!style) return 0;
    int plen = (int)strlen(prop);
    const char *s = style->val, *end = style->val + style->vlen;
    while (s < end) {
        while (s < end && (svg_isspace(*s) || *s == ';')) s++;
        const char *name = s;
        while (s < end && *s != ':' && *s != ';') s++;
        int nlen = (int)(s - name);
        while (nlen > 0 && svg_isspace(name[nlen - 1])) nlen--;
        if (s < end && *s == ':') {
            s++;
            while (s < end && svg_isspace(*s)) s++;
            const char *val = s;
            while (s < end && *s != ';') s++;
            int vlen = (int)(s - val);
            while (vlen > 0 && svg_isspace(val[vlen - 1])) vlen--;
            if (nlen == plen && svg_ci_eq(name, prop, plen)) {
                if (vlen > max - 1) vlen = max - 1;
                for (int i = 0; i < vlen; i++) out[i] = val[i];
                out[vlen] = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* One property, from the style attribute if it is there and the presentation
 * attribute otherwise -- the style attribute wins, as the cascade says. */
static int svg_prop(const svg_attr *at, int nat, const char *name,
                    char *out, int max) {
    const svg_attr *style = svg_find(at, nat, "style");
    if (svg_style_get(style, name, out, max)) return 1;
    const svg_attr *a = svg_find(at, nat, name);
    if (!a) return 0;
    svg_attr_str(a, out, max);
    return 1;
}

/* A length, resolved against a reference for percentages. px is the only unit
 * the drawings we meet actually carry. */
static svg_fx svg_len(const svg_attr *a, svg_fx ref, svg_fx dflt) {
    if (!a) return dflt;
    char buf[64];
    svg_attr_str(a, buf, sizeof buf);
    const char *p = buf;
    svg_fx v = svg_number(&p);
    while (svg_isspace(*p)) p++;
    if (*p == '%') return fx_mul(v, ref) / 100;
    return v;
}

/* An opacity: a number, or a percentage. 0..255. */
static int svg_opacity(const char *s) {
    const char *p = s;
    svg_fx v = svg_number(&p);
    while (svg_isspace(*p)) p++;
    if (*p == '%') v /= 100;
    int o = (int)((v * 255 + FX_HALF) >> 16);
    return o < 0 ? 0 : o > 255 ? 255 : o;
}

/* transform="translate(..) scale(..) rotate(..) matrix(..) skewX(..) skewY(..)" */
static svg_mat svg_transform(const char *s) {
    svg_mat m = rast_identity();
    while (*s) {
        while (*s && (svg_isspace(*s) || *s == ',')) s++;
        if (!*s) break;
        const char *name = s;
        while (*s && *s != '(' && !svg_isspace(*s)) s++;
        int nlen = (int)(s - name);
        while (svg_isspace(*s)) s++;
        if (*s != '(') break;
        s++;

        svg_fx v[6] = { 0, 0, 0, 0, 0, 0 };
        int nv = 0;
        while (*s && *s != ')' && nv < 6) {
            v[nv++] = svg_number(&s);
            svg_skip_sep(&s);
        }
        while (*s && *s != ')') s++;
        if (*s == ')') s++;

        svg_mat t = rast_identity();
        if (nlen == 9 && svg_ci_eq(name, "translate", 9)) {
            t = rast_translate(v[0], nv > 1 ? v[1] : 0);
        } else if (nlen == 5 && svg_ci_eq(name, "scale", 5)) {
            t = rast_scale(v[0], nv > 1 ? v[1] : v[0]);
        } else if (nlen == 6 && svg_ci_eq(name, "rotate", 6)) {
            t = rast_rotate(v[0]);
            if (nv >= 3)            /* rotate about a point */
                t = rast_mul(rast_translate(v[1], v[2]),
                             rast_mul(t, rast_translate(-v[1], -v[2])));
        } else if (nlen == 6 && svg_ci_eq(name, "matrix", 6)) {
            t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5];
        } else if (nlen == 5 && svg_ci_eq(name, "skewX", 5)) {
            t = rast_skew(v[0], 0);
        } else if (nlen == 5 && svg_ci_eq(name, "skewY", 5)) {
            t = rast_skew(0, v[0]);
        } else {
            continue;
        }
        m = rast_mul(m, t);
    }
    return m;
}

/* The id a url(#id) or an href="#id" names: its start and length, 0 if none. */
static int svg_ref(const char *s, const char **id) {
    while (svg_isspace(*s)) s++;
    if (svg_ci_eq(s, "url(", 4)) {
        s += 4;
        while (svg_isspace(*s) || *s == '\'' || *s == '"') s++;
    }
    if (*s != '#') return 0;
    s++;
    int n = 0;
    while (s[n] && s[n] != ')' && s[n] != '\'' && s[n] != '"' && !svg_isspace(s[n])) n++;
    *id = s;
    return n;
}

/* The end of an element that has children: past its closing tag, or the
 * end of the document. Same-named elements nested inside are counted. */
static const char *svg_skip_element(const char *p, const char *name, int nl) {
    int depth = 1;
    while (*p) {
        if (p[0] == '<') {
            if (p[1] == '/' && svg_ci_eq(p + 2, name, nl) &&
                (p[2 + nl] == '>' || svg_isspace(p[2 + nl]))) {
                if (--depth == 0) {
                    while (*p && *p != '>') p++;
                    return *p ? p + 1 : p;
                }
            } else if (svg_ci_eq(p + 1, name, nl) &&
                       (svg_isspace(p[1 + nl]) || p[1 + nl] == '>')) {
                const char *q = p + 1 + nl;
                while (*q && *q != '>') q++;
                if (q > p && q[-1] != '/') depth++;
            }
        }
        p++;
    }
    return p;
}

/* ==========================================================================
 * gradients
 * ========================================================================== */

#define SVG_MAX_GRADS 96
#define SVG_MAX_STOPS 32

enum { G_X1, G_Y1, G_X2, G_Y2, G_CX, G_CY, G_R, G_FX, G_FY, G_NV,
       G_UNITS = G_NV, G_XFORM, G_SPREAD };

typedef struct {
    char      id[64];
    char      href[64];
    int       type;                /* RAST_LINEAR or RAST_RADIAL */
    unsigned  set;                 /* which fields were given, by bit */
    unsigned  pct;                 /* which of v[] were percentages */
    svg_fx    v[G_NV];
    int       obb;                 /* gradientUnits="objectBoundingBox" */
    svg_mat   m;                   /* gradientTransform */
    int       spread;
    rast_stop stops[SVG_MAX_STOPS];
    int       nstops;
} svg_grad;

static void svg_copy_id(char *dst, int cap, const char *s, int n) {
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, s, (size_t)n);
    dst[n] = 0;
}

/* One <linearGradient> or <radialGradient> and its <stop>s. `p` is just past
 * the tag name, `type` which of the two it is. Returns where it ends. */
static const char *svg_read_grad(svg_grad *G, const char *p, int type,
                                 const char *name, int nl) {
    memset(G, 0, sizeof *G);
    G->type = type;
    G->obb = 1;
    G->m = rast_identity();
    G->spread = RAST_PAD;

    svg_attr at[SVG_ATTR_MAX];
    const char *ap = p;
    int nat = svg_attrs(&ap, at, SVG_ATTR_MAX);
    int selfclose = (*ap == '/');
    while (*ap && *ap != '>') ap++;
    if (*ap) ap++;

    const svg_attr *a;
    if ((a = svg_find(at, nat, "id"))) svg_copy_id(G->id, sizeof G->id, a->val, a->vlen);
    if ((a = svg_find(at, nat, "xlink:href")) || (a = svg_find(at, nat, "href"))) {
        char b[80];
        svg_attr_str(a, b, sizeof b);
        const char *id;
        int n = svg_ref(b, &id);
        if (n) svg_copy_id(G->href, sizeof G->href, id, n);
    }
    static const char *const names[G_NV] = { "x1", "y1", "x2", "y2", "cx", "cy", "r", "fx", "fy" };
    for (int i = 0; i < G_NV; i++) {
        if (!(a = svg_find(at, nat, names[i]))) continue;
        char b[48];
        svg_attr_str(a, b, sizeof b);
        const char *q = b;
        G->v[i] = svg_number(&q);
        while (svg_isspace(*q)) q++;
        if (*q == '%') G->pct |= 1u << i;
        G->set |= 1u << i;
    }
    if ((a = svg_find(at, nat, "gradientUnits"))) {
        G->obb = !(a->vlen >= 14 && svg_ci_eq(a->val, "userSpaceOnUse", 14));
        G->set |= 1u << G_UNITS;
    }
    if ((a = svg_find(at, nat, "gradientTransform"))) {
        char t[512];
        svg_attr_str(a, t, sizeof t);
        G->m = svg_transform(t);
        G->set |= 1u << G_XFORM;
    }
    if ((a = svg_find(at, nat, "spreadMethod"))) {
        G->spread = svg_ci_eq(a->val, "reflect", 7) ? RAST_REFLECT
                  : svg_ci_eq(a->val, "repeat", 6) ? RAST_REPEAT : RAST_PAD;
        G->set |= 1u << G_SPREAD;
    }
    if (selfclose) return ap;

    const char *end = svg_skip_element(ap, name, nl);
    svg_fx last = 0;
    for (const char *s = ap; s < end; s++) {
        if (s[0] != '<' || !svg_ci_eq(s + 1, "stop", 4) ||
            !(svg_isspace(s[5]) || s[5] == '/' || s[5] == '>')) continue;
        const char *sp = s + 5;
        svg_attr sa[SVG_ATTR_MAX];
        int ns = svg_attrs(&sp, sa, SVG_ATTR_MAX);
        if (G->nstops >= SVG_MAX_STOPS) continue;
        char b[64];
        svg_fx off = 0;
        if ((a = svg_find(sa, ns, "offset"))) {
            svg_attr_str(a, b, sizeof b);
            const char *q = b;
            off = svg_number(&q);
            while (svg_isspace(*q)) q++;
            if (*q == '%') off /= 100;
        }
        if (off < 0) off = 0;
        if (off > FX_ONE) off = FX_ONE;
        if (off < last) off = last;                /* offsets never go back */
        last = off;
        unsigned col = 0xFF000000u;
        if (svg_prop(sa, ns, "stop-color", b, sizeof b)) col = svg_color(b, 0xFF000000u);
        int op = 255;
        if (svg_prop(sa, ns, "stop-opacity", b, sizeof b)) op = svg_opacity(b);
        unsigned alpha = ((col >> 24) * (unsigned)op + 127) / 255;
        G->stops[G->nstops].offset = off;
        G->stops[G->nstops].color = (alpha << 24) | (col & 0xFFFFFF);
        G->nstops++;
    }
    return end;
}

/* ==========================================================================
 * the drawing state, inherited down the tree
 * ========================================================================== */

#define SVG_MAX_DASH 16

typedef struct {
    svg_mat  m;
    unsigned fill, stroke;      /* 0xAARRGGBB; A == 0 means not painted */
    int      fill_grad, stroke_grad;   /* a gradient instead, or -1 */
    unsigned color;             /* what currentColor is */
    int      fill_op, stroke_op;/* 0..255 */
    int      opacity;           /* 0..255, multiplied down the tree */
    svg_fx   swidth;
    int      evenodd, clip_evenodd;
    int      join, cap;
    svg_fx   miter;
    svg_fx   dash[SVG_MAX_DASH];
    int      ndash;
    svg_fx   dashoff;
    unsigned char *mask;        /* the clip in effect, or NULL */
    unsigned char *own_mask;    /* the one this level made, freed with it */
} svg_state;

#define SVG_DEPTH 48
#define SVG_MAX_CLIPS 48

typedef struct {
    char        id[64];
    const char *body, *end;     /* its children, in the document */
    int         obb;            /* clipPathUnits="objectBoundingBox" */
    svg_mat     m;              /* its own transform */
} svg_clip;

typedef struct {
    unsigned   *px;             /* premultiplied ARGB */
    int         w, h;
    rast_target t;
    svg_state   st[SVG_DEPTH];
    int         depth;
    int         skip_depth;     /* >0 while inside something we cannot draw */
    svg_fx      vbw, vbh;       /* for percentages */
    svg_grad   *grads;
    int         ngrads;
    svg_clip    clips[SVG_MAX_CLIPS];
    int         nclips;
    int         in_clip;        /* building a clip path's mask */
} svg_ctx;

static int svg_find_grad(svg_ctx *C, const char *id, int n) {
    for (int i = 0; i < C->ngrads; i++)
        if ((int)strlen(C->grads[i].id) == n && !memcmp(C->grads[i].id, id, (size_t)n)) return i;
    return -1;
}

/* A paint: "none", a colour, or url(#id) with an optional fallback. */
static void svg_paint_prop(svg_ctx *C, const char *v, unsigned *col, int *grad,
                           unsigned current) {
    const char *id;
    while (svg_isspace(*v)) v++;
    if (svg_ci_eq(v, "url(", 4)) {
        int n = svg_ref(v, &id);
        int g = n ? svg_find_grad(C, id, n) : -1;
        if (g >= 0) { *grad = g; *col = 0; return; }
        /* Not a gradient we have: the fallback after it, or nothing. */
        const char *f = v;
        while (*f && *f != ')') f++;
        if (*f == ')') f++;
        while (svg_isspace(*f)) f++;
        *grad = -1;
        *col = *f ? svg_color(f, current) : 0;
        return;
    }
    *grad = -1;
    *col = svg_color(v, current);
}

static void svg_state_apply(svg_ctx *C, svg_state *st, const svg_attr *at, int nat) {
    char buf[160];

    const svg_attr *tr = svg_find(at, nat, "transform");
    if (tr) {
        char t[512];
        svg_attr_str(tr, t, sizeof t);
        st->m = rast_mul(st->m, svg_transform(t));
    }

    if (svg_prop(at, nat, "color", buf, sizeof buf))
        st->color = svg_color(buf, st->color);
    if (svg_prop(at, nat, "fill", buf, sizeof buf))
        svg_paint_prop(C, buf, &st->fill, &st->fill_grad, st->color);
    if (svg_prop(at, nat, "stroke", buf, sizeof buf))
        svg_paint_prop(C, buf, &st->stroke, &st->stroke_grad, st->color);

    if (svg_prop(at, nat, "stroke-width", buf, sizeof buf)) {
        const char *p = buf;
        st->swidth = svg_number(&p);
        while (svg_isspace(*p)) p++;
        if (*p == '%') st->swidth = fx_mul(st->swidth, C->vbw) / 100;
    }
    if (svg_prop(at, nat, "fill-rule", buf, sizeof buf))
        st->evenodd = svg_ci_eq(buf, "evenodd", 7);
    if (svg_prop(at, nat, "clip-rule", buf, sizeof buf))
        st->clip_evenodd = svg_ci_eq(buf, "evenodd", 7);
    if (svg_prop(at, nat, "stroke-linejoin", buf, sizeof buf))
        st->join = svg_ci_eq(buf, "round", 5) ? RAST_JOIN_ROUND
                 : svg_ci_eq(buf, "bevel", 5) ? RAST_JOIN_BEVEL : RAST_JOIN_MITER;
    if (svg_prop(at, nat, "stroke-linecap", buf, sizeof buf))
        st->cap = svg_ci_eq(buf, "round", 5) ? RAST_CAP_ROUND
                : svg_ci_eq(buf, "square", 6) ? RAST_CAP_SQUARE : RAST_CAP_BUTT;
    if (svg_prop(at, nat, "stroke-miterlimit", buf, sizeof buf)) {
        const char *p = buf;
        svg_fx v = svg_number(&p);
        if (v >= FX_ONE) st->miter = v;
    }
    if (svg_prop(at, nat, "stroke-dasharray", buf, sizeof buf)) {
        st->ndash = 0;
        if (!svg_ci_eq(buf, "none", 4)) {
            const char *p = buf;
            int bad = 0;
            while (*p && st->ndash < SVG_MAX_DASH) {
                svg_skip_sep(&p);
                if (!*p) break;
                const char *before = p;
                svg_fx v = svg_number(&p);
                if (p == before) { bad = 1; break; }
                if (*p == '%') { v = fx_mul(v, C->vbw) / 100; p++; }
                if (v < 0) bad = 1;
                st->dash[st->ndash++] = v;
            }
            if (bad) st->ndash = 0;
        }
    }
    if (svg_prop(at, nat, "stroke-dashoffset", buf, sizeof buf)) {
        const char *p = buf;
        st->dashoff = svg_number(&p);
    }

    /* The three opacities: group opacity is multiplied into everything below
     * -- not the same as drawing the group and then fading it where shapes
     * overlap, but the same everywhere else. */
    if (svg_prop(at, nat, "opacity", buf, sizeof buf))
        st->opacity = st->opacity * svg_opacity(buf) / 255;
    if (svg_prop(at, nat, "fill-opacity", buf, sizeof buf))
        st->fill_op = svg_opacity(buf);
    if (svg_prop(at, nat, "stroke-opacity", buf, sizeof buf))
        st->stroke_op = svg_opacity(buf);
}

/* ==========================================================================
 * shapes
 * ========================================================================== */

static void svg_parse_path(rast_path *P, const char *d) {
    svg_fx cx = 0, cy = 0;         /* current point            */
    svg_fx sx = 0, sy = 0;         /* start of the subpath     */
    svg_fx rx = 0, ry = 0;         /* the last control point, for S and T */
    int    have_ctrl = 0;          /* 1 after C or S, 2 after Q or T */
    char   cmd = 0;

    const char *p = d;
    for (;;) {
        svg_skip_sep(&p);
        if (!*p) break;

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
        } else if (!cmd) {
            p++;                    /* junk before any command */
            continue;
        } else if (cmd == 'M') {
            cmd = 'L';              /* extra pairs after a moveto are linetos */
        } else if (cmd == 'm') {
            cmd = 'l';
        }

        int rel = (cmd >= 'a' && cmd <= 'z');
        char c = rel ? (char)(cmd - 32) : cmd;
        svg_fx ox = rel ? cx : 0, oy = rel ? cy : 0;
        const char *before = p;

        switch (c) {
        case 'M': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            rast_move_to(P, x, y);
            cx = sx = x; cy = sy = y;
            have_ctrl = 0;
            break;
        }
        case 'L': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            rast_line_to(P, x, y);
            cx = x; cy = y; have_ctrl = 0;
            break;
        }
        case 'H': {
            svg_fx x = svg_number(&p) + ox;
            rast_line_to(P, x, cy);
            cx = x; have_ctrl = 0;
            break;
        }
        case 'V': {
            svg_fx y = svg_number(&p) + oy;
            rast_line_to(P, cx, y);
            cy = y; have_ctrl = 0;
            break;
        }
        case 'C': {
            svg_fx x1 = svg_number(&p) + ox, y1 = svg_number(&p) + oy;
            svg_fx x2 = svg_number(&p) + ox, y2 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            rast_cubic_to(P, x1, y1, x2, y2, x, y);
            rx = x2; ry = y2; have_ctrl = 1;
            cx = x; cy = y;
            break;
        }
        case 'S': {
            svg_fx x2 = svg_number(&p) + ox, y2 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            svg_fx x1 = have_ctrl == 1 ? 2 * cx - rx : cx;
            svg_fx y1 = have_ctrl == 1 ? 2 * cy - ry : cy;
            rast_cubic_to(P, x1, y1, x2, y2, x, y);
            rx = x2; ry = y2; have_ctrl = 1;
            cx = x; cy = y;
            break;
        }
        case 'Q': {
            svg_fx x1 = svg_number(&p) + ox, y1 = svg_number(&p) + oy;
            svg_fx x  = svg_number(&p) + ox, y  = svg_number(&p) + oy;
            rast_quad_to(P, x1, y1, x, y);
            rx = x1; ry = y1; have_ctrl = 2;
            cx = x; cy = y;
            break;
        }
        case 'T': {
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            svg_fx x1 = (have_ctrl == 2) ? 2 * cx - rx : cx;
            svg_fx y1 = (have_ctrl == 2) ? 2 * cy - ry : cy;
            rast_quad_to(P, x1, y1, x, y);
            rx = x1; ry = y1; have_ctrl = 2;
            cx = x; cy = y;
            break;
        }
        case 'A': {
            svg_fx arx = svg_number(&p), ary = svg_number(&p);
            svg_fx rot = svg_number(&p);
            int large = svg_flag(&p), sweep = svg_flag(&p);
            svg_fx x = svg_number(&p) + ox, y = svg_number(&p) + oy;
            rast_arc_to(P, arx, ary, rot, large, sweep, x, y);
            cx = x; cy = y; have_ctrl = 0;
            break;
        }
        case 'Z':
            rast_close(P);
            cx = sx; cy = sy;
            have_ctrl = 0;
            break;
        default:
            /* An unknown letter: skip to the next one rather than spin. */
            while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
            cmd = 0;
            break;
        }
        /* A command letter with no numbers after it where numbers belong
         * would repeat for ever; move on instead. */
        if (c != 'Z' && p == before && *p && !((*p >= 'A' && *p <= 'Z') ||
                                               (*p >= 'a' && *p <= 'z')))
            p++;
    }
}

static void svg_points_path(rast_path *P, const char *s, int close) {
    int first = 1;
    for (;;) {
        svg_skip_sep(&s);
        if (!*s) break;
        if (!svg_isdigit(*s) && *s != '-' && *s != '+' && *s != '.') break;
        svg_fx x = svg_number(&s), y = svg_number(&s);
        if (first) { rast_move_to(P, x, y); first = 0; }
        else rast_line_to(P, x, y);
    }
    if (!first && close) rast_close(P);
}

/* The shape an element describes, into P. 0 if it is not a shape, or an
 * empty one. `nofill` is set for the two that are never filled. */
static int svg_shape(svg_ctx *C, const char *name, int nl, const svg_attr *at,
                     int nat, rast_path *P, int *nofill) {
    svg_fx vbw = C->vbw, vbh = C->vbh;
    *nofill = 0;
    if (nl == 4 && svg_ci_eq(name, "path", 4)) {
        const svg_attr *d = svg_find(at, nat, "d");
        if (!d) return 0;
        char *dd = (char *)malloc((size_t)d->vlen + 1);
        if (!dd) return 0;
        memcpy(dd, d->val, (size_t)d->vlen);
        dd[d->vlen] = 0;
        svg_parse_path(P, dd);
        free(dd);
    } else if (nl == 4 && svg_ci_eq(name, "rect", 4)) {
        svg_fx x = svg_len(svg_find(at, nat, "x"), vbw, 0);
        svg_fx y = svg_len(svg_find(at, nat, "y"), vbh, 0);
        svg_fx w = svg_len(svg_find(at, nat, "width"),  vbw, 0);
        svg_fx h = svg_len(svg_find(at, nat, "height"), vbh, 0);
        const svg_attr *arx = svg_find(at, nat, "rx"), *ary = svg_find(at, nat, "ry");
        svg_fx r1 = svg_len(arx, vbw, -1), r2 = svg_len(ary, vbh, -1);
        if (r1 < 0) r1 = r2 < 0 ? 0 : r2;
        if (r2 < 0) r2 = r1;
        if (w <= 0 || h <= 0) return 0;
        rast_round_rect(P, x, y, w, h, r1, r2);
    } else if (nl == 6 && svg_ci_eq(name, "circle", 6)) {
        svg_fx cx = svg_len(svg_find(at, nat, "cx"), vbw, 0);
        svg_fx cy = svg_len(svg_find(at, nat, "cy"), vbh, 0);
        svg_fx r  = svg_len(svg_find(at, nat, "r"),  vbw, 0);
        if (r <= 0) return 0;
        rast_ellipse(P, cx, cy, r, r);
    } else if (nl == 7 && svg_ci_eq(name, "ellipse", 7)) {
        svg_fx cx = svg_len(svg_find(at, nat, "cx"), vbw, 0);
        svg_fx cy = svg_len(svg_find(at, nat, "cy"), vbh, 0);
        svg_fx r1 = svg_len(svg_find(at, nat, "rx"), vbw, 0);
        svg_fx r2 = svg_len(svg_find(at, nat, "ry"), vbh, 0);
        if (r1 <= 0 || r2 <= 0) return 0;
        rast_ellipse(P, cx, cy, r1, r2);
    } else if (nl == 4 && svg_ci_eq(name, "line", 4)) {
        rast_move_to(P, svg_len(svg_find(at, nat, "x1"), vbw, 0),
                        svg_len(svg_find(at, nat, "y1"), vbh, 0));
        rast_line_to(P, svg_len(svg_find(at, nat, "x2"), vbw, 0),
                        svg_len(svg_find(at, nat, "y2"), vbh, 0));
        *nofill = 1;
    } else if ((nl == 7 && svg_ci_eq(name, "polygon", 7)) ||
               (nl == 8 && svg_ci_eq(name, "polyline", 8))) {
        const svg_attr *pts = svg_find(at, nat, "points");
        if (!pts) return 0;
        char *s2 = (char *)malloc((size_t)pts->vlen + 1);
        if (!s2) return 0;
        memcpy(s2, pts->val, (size_t)pts->vlen);
        s2[pts->vlen] = 0;
        svg_points_path(P, s2, nl == 7);
        free(s2);
        if (nl == 8) *nofill = 1;
    } else {
        return 0;
    }
    return P->nops > 0;
}

/* ==========================================================================
 * painting
 * ========================================================================== */

/* The gradient at index g with everything it inherits filled in. */
static void svg_resolve_grad(svg_ctx *C, int g, svg_grad *out) {
    *out = C->grads[g];
    const svg_grad *src = &C->grads[g];
    for (int hops = 0; hops < 8 && src->href[0]; hops++) {
        int h = svg_find_grad(C, src->href, (int)strlen(src->href));
        if (h < 0 || h == g) break;
        src = &C->grads[h];
        for (int i = 0; i < G_NV; i++)
            if (!(out->set & (1u << i)) && (src->set & (1u << i))) {
                out->v[i] = src->v[i];
                out->pct = (out->pct & ~(1u << i)) | (src->pct & (1u << i));
                out->set |= 1u << i;
            }
        if (!(out->set & (1u << G_UNITS)) && (src->set & (1u << G_UNITS))) {
            out->obb = src->obb; out->set |= 1u << G_UNITS;
        }
        if (!(out->set & (1u << G_XFORM)) && (src->set & (1u << G_XFORM))) {
            out->m = src->m; out->set |= 1u << G_XFORM;
        }
        if (!(out->set & (1u << G_SPREAD)) && (src->set & (1u << G_SPREAD))) {
            out->spread = src->spread; out->set |= 1u << G_SPREAD;
        }
        if (!out->nstops && src->nstops) {
            memcpy(out->stops, src->stops, sizeof out->stops);
            out->nstops = src->nstops;
        }
    }
}

/* A coordinate of a gradient: a percentage of the box it is measured in. */
static svg_fx svg_gv(const svg_grad *G, int i, svg_fx dflt_pct, svg_fx ref) {
    svg_fx v = (G->set & (1u << i)) ? G->v[i] : dflt_pct;
    int pct = (G->set & (1u << i)) ? !!(G->pct & (1u << i)) : 1;
    if (!pct) return v;
    return G->obb ? v / 100 : fx_mul(v, ref) / 100;
}

/* What fills (or strokes) this shape, as librast wants it. 0 for nothing. */
static int svg_paint(svg_ctx *C, const svg_state *st, int fill, const rast_path *P,
                     rast_paint *pt, svg_grad *G) {
    int op = (fill ? st->fill_op : st->stroke_op) * st->opacity / 255;
    if (op <= 0) return 0;
    int g = fill ? st->fill_grad : st->stroke_grad;
    if (g < 0) {
        unsigned c = fill ? st->fill : st->stroke;
        if (!(c >> 24)) return 0;
        rast_paint_solid(pt, c);
        pt->opacity = op;
        return 1;
    }
    svg_resolve_grad(C, g, G);
    if (!G->nstops) return 0;                /* no stops: paints nothing */

    memset(pt, 0, sizeof *pt);
    pt->type = G->type;
    pt->stops = G->stops;
    pt->nstops = G->nstops;
    pt->spread = G->spread;
    pt->opacity = op;
    pt->m = G->m;
    if (G->obb) {
        svg_fx box[4];
        if (!rast_path_bounds(P, box)) return 0;
        svg_fx bw = box[2] - box[0], bh = box[3] - box[1];
        if (bw <= 0 || bh <= 0) return 0;     /* the specification: not rendered */
        svg_mat bm = { bw, 0, 0, bh, box[0], box[1] };
        pt->m = rast_mul(bm, G->m);
    }
    svg_fx diag = C->vbw > C->vbh ? C->vbw : C->vbh;
    if (G->type == RAST_LINEAR) {
        pt->x1 = svg_gv(G, G_X1, 0, C->vbw);
        pt->y1 = svg_gv(G, G_Y1, 0, C->vbh);
        pt->x2 = svg_gv(G, G_X2, 100 * FX_ONE, C->vbw);
        pt->y2 = svg_gv(G, G_Y2, 0, C->vbh);
    } else {
        pt->cx = svg_gv(G, G_CX, 50 * FX_ONE, C->vbw);
        pt->cy = svg_gv(G, G_CY, 50 * FX_ONE, C->vbh);
        pt->r  = svg_gv(G, G_R,  50 * FX_ONE, diag);
        pt->fx = (G->set & (1u << G_FX)) ? svg_gv(G, G_FX, 0, C->vbw) : pt->cx;
        pt->fy = (G->set & (1u << G_FY)) ? svg_gv(G, G_FY, 0, C->vbh) : pt->cy;
    }
    return 1;
}

static void svg_draw(svg_ctx *C, const rast_path *P, const svg_state *st, int nofill) {
    rast_paint pt;
    svg_grad *G = (svg_grad *)malloc(sizeof *G);
    if (!G) return;
    C->t.mask = st->mask;
    C->t.mask_stride = C->w;

    if (!nofill && svg_paint(C, st, 1, P, &pt, G))
        rast_fill(&C->t, P, &st->m, st->evenodd ? RAST_EVENODD : RAST_NONZERO, &pt);

    if (st->swidth > 0 && svg_paint(C, st, 0, P, &pt, G)) {
        rast_stroke s;
        rast_stroke_init(&s, st->swidth);
        s.join = st->join;
        s.cap = st->cap;
        s.miter_limit = st->miter;
        if (st->ndash > 0) {
            s.dash = st->dash;
            s.ndash = st->ndash;
            s.dash_offset = st->dashoff;
        }
        rast_draw_stroke(&C->t, P, &st->m, &s, &pt);
    }
    C->t.mask = NULL;
    free(G);
}

/* ==========================================================================
 * the tree
 * ========================================================================== */

/* Elements whose contents describe something other than what is drawn. Their
 * whole subtree is stepped over; the ones that matter were read beforehand. */
static int svg_is_skipped(const char *n, int len) {
    static const char *skip[] = { "defs", "clipPath", "mask", "pattern",
                                  "linearGradient", "radialGradient", "filter",
                                  "style", "text", "title", "desc", "metadata",
                                  "symbol", "marker", "switch", "foreignObject",
                                  "script" };
    for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) {
        int l = (int)strlen(skip[i]);
        if (l == len && svg_ci_eq(n, skip[i], l)) return 1;
    }
    return 0;
}

static void svg_walk(svg_ctx *C, const char *p, const char *end, unsigned char *clipmask);

/* The mask a clip-path reference makes, for an element drawn with state st
 * whose own geometry (for objectBoundingBox) is P, or NULL. Intersected with
 * any clip already in effect. */
static unsigned char *svg_clip_mask(svg_ctx *C, const svg_attr *at, int nat,
                                    const svg_state *st, const rast_path *P) {
    char buf[96];
    if (!svg_prop(at, nat, "clip-path", buf, sizeof buf)) return NULL;
    const char *id;
    int n = svg_ref(buf, &id);
    if (!n) return NULL;
    const svg_clip *K = NULL;
    for (int i = 0; i < C->nclips; i++)
        if ((int)strlen(C->clips[i].id) == n && !memcmp(C->clips[i].id, id, (size_t)n)) {
            K = &C->clips[i];
            break;
        }
    if (!K) return NULL;
    if (C->depth + 2 >= SVG_DEPTH) return NULL;

    unsigned char *mask = (unsigned char *)calloc((size_t)C->w * C->h, 1);
    if (!mask) return NULL;

    /* The clip's children are drawn in the referencing element's space, then
     * through the clip's own transform -- and for objectBoundingBox through
     * the element's box as well. */
    svg_state base = *st;
    base.m = rast_mul(st->m, K->m);
    if (K->obb) {
        /* A group's box would need every shape in it measured first; the
         * clip is left off rather than hiding the whole group. */
        if (!P) { free(mask); return NULL; }
        svg_fx box[4];
        if (!rast_path_bounds(P, box) || box[2] <= box[0] || box[3] <= box[1])
            return mask;                       /* nothing survives the clip */
        svg_mat bm = { box[2] - box[0], 0, 0, box[3] - box[1], box[0], box[1] };
        base.m = rast_mul(base.m, bm);
    }
    base.mask = NULL;
    base.own_mask = NULL;
    base.clip_evenodd = 0;

    int save_depth = C->depth, save_skip = C->skip_depth, save_in = C->in_clip;
    C->st[++C->depth] = base;
    C->skip_depth = 0;
    C->in_clip = 1;
    svg_walk(C, K->body, K->end, mask);
    C->depth = save_depth;
    C->skip_depth = save_skip;
    C->in_clip = save_in;

    if (st->mask) {
        long total = (long)C->w * C->h;
        for (long i = 0; i < total; i++)
            mask[i] = (unsigned char)((mask[i] * st->mask[i] + 127) / 255);
    }
    return mask;
}

static void svg_pop(svg_ctx *C) {
    if (C->depth <= 0) return;
    free(C->st[C->depth].own_mask);
    C->st[C->depth].own_mask = NULL;
    C->depth--;
}

/* Walk the elements from p to end, drawing -- or, with clipmask, adding the
 * coverage of every shape into it. */
static void svg_walk(svg_ctx *C, const char *p, const char *end, unsigned char *clipmask) {
    int base_depth = C->depth;
    while (p < end && *p) {
        if (*p != '<') { p++; continue; }

        if (p[1] == '!') {                      /* comment, doctype, CDATA */
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : p + strlen(p);
            } else {
                while (*p && *p != '>') p++;
                if (*p) p++;
            }
            continue;
        }
        if (p[1] == '?') {
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }

        if (p[1] == '/') {                      /* a closing tag */
            if (C->skip_depth > 0) C->skip_depth--;
            else if (C->depth > base_depth) svg_pop(C);
            while (*p && *p != '>') p++;
            if (*p) p++;
            continue;
        }

        const char *name = p + 1;
        int nl = 0;
        while (name[nl] && name[nl] != '>' && name[nl] != '/' && !svg_isspace(name[nl])) nl++;

        const char *ap = name + nl;
        svg_attr at[SVG_ATTR_MAX];
        int nat = svg_attrs(&ap, at, SVG_ATTR_MAX);
        int selfclose = (*ap == '/');
        while (*ap && *ap != '>') ap++;
        if (*ap) ap++;
        const char *after = ap;

        if (C->skip_depth > 0) {
            if (!selfclose) C->skip_depth++;
            p = after;
            continue;
        }
        if (svg_is_skipped(name, nl)) {
            if (!selfclose) C->skip_depth = 1;
            p = after;
            continue;
        }

        /* display="none" and visibility="hidden" draw nothing below them. */
        char vis[32];
        if ((svg_prop(at, nat, "display", vis, sizeof vis) && svg_ci_eq(vis, "none", 4)) ||
            (svg_prop(at, nat, "visibility", vis, sizeof vis) &&
             (svg_ci_eq(vis, "hidden", 6) || svg_ci_eq(vis, "collapse", 8)))) {
            if (!selfclose) C->skip_depth = 1;
            p = after;
            continue;
        }

        /* Everything inherits, so the child's state starts as a copy. */
        svg_state st = C->st[C->depth];
        st.own_mask = NULL;
        svg_state_apply(C, &st, at, nat);

        int container = (nl == 1 && (name[0] == 'g' || name[0] == 'G')) ||
                        (nl == 3 && svg_ci_eq(name, "svg", 3)) ||
                        (nl == 1 && (name[0] == 'a' || name[0] == 'A'));

        if (container) {
            if (!clipmask) {
                unsigned char *m = svg_clip_mask(C, at, nat, &st, NULL);
                if (m) { st.mask = m; st.own_mask = m; }
            }
            if (!selfclose && C->depth + 1 < SVG_DEPTH) C->st[++C->depth] = st;
            else {
                free(st.own_mask);
                if (!selfclose) C->skip_depth = 1;     /* nested too deep */
            }
            p = after;
            continue;
        }

        rast_path P;
        rast_path_init(&P);
        int nofill = 0;
        if (svg_shape(C, name, nl, at, nat, &P, &nofill)) {
            if (clipmask) {
                rast_fill_mask(clipmask, C->w, C->h, C->w, &P, &st.m,
                               st.clip_evenodd ? RAST_EVENODD : RAST_NONZERO);
            } else {
                unsigned char *m = svg_clip_mask(C, at, nat, &st, &P);
                if (m) st.mask = m;
                svg_draw(C, &P, &st, nofill);
                free(m);
            }
        }
        rast_path_free(&P);
        /* A shape written with a separate closing tag: step over it. */
        if (!selfclose) C->skip_depth = 1;
        p = after;
    }
    while (C->depth > base_depth) svg_pop(C);
}

/* Everything that is referred to rather than drawn: gradients and clip
 * paths, wherever in the document they are. */
static void svg_collect(svg_ctx *C, const char *doc) {
    for (const char *p = doc; *p; p++) {
        if (*p != '<' || p[1] == '/' || p[1] == '!' || p[1] == '?') continue;
        const char *name = p + 1;
        int nl = 0;
        while (name[nl] && name[nl] != '>' && name[nl] != '/' && !svg_isspace(name[nl])) nl++;

        int type = (nl == 14 && svg_ci_eq(name, "linearGradient", 14)) ? RAST_LINEAR
                 : (nl == 14 && svg_ci_eq(name, "radialGradient", 14)) ? RAST_RADIAL : -1;
        if (type >= 0) {
            if (C->ngrads < SVG_MAX_GRADS) {
                svg_read_grad(&C->grads[C->ngrads], name + nl, type, name, nl);
                if (C->grads[C->ngrads].id[0]) C->ngrads++;
            }
            continue;
        }
        if (nl == 8 && svg_ci_eq(name, "clipPath", 8) && C->nclips < SVG_MAX_CLIPS) {
            const char *ap = name + nl;
            svg_attr at[SVG_ATTR_MAX];
            int nat = svg_attrs(&ap, at, SVG_ATTR_MAX);
            int selfclose = (*ap == '/');
            while (*ap && *ap != '>') ap++;
            if (*ap) ap++;
            const svg_attr *a = svg_find(at, nat, "id");
            if (!a || selfclose) continue;
            svg_clip *K = &C->clips[C->nclips];
            memset(K, 0, sizeof *K);
            svg_copy_id(K->id, sizeof K->id, a->val, a->vlen);
            K->m = rast_identity();
            if ((a = svg_find(at, nat, "transform"))) {
                char t[512];
                svg_attr_str(a, t, sizeof t);
                K->m = svg_transform(t);
            }
            a = svg_find(at, nat, "clipPathUnits");
            K->obb = a && a->vlen >= 17 && svg_ci_eq(a->val, "objectBoundingBox", 17);
            K->body = ap;
            K->end = svg_skip_element(ap, "clipPath", 8);
            C->nclips++;
        }
    }
}

/* How big should this drawing be rendered? The width and height attributes if
 * it has them, the viewBox if not, and a default if neither -- then clamped so
 * a drawing that claims to be ten thousand pixels wide does not try to be. */
#define SVG_MIN_SIDE 1
#define SVG_MAX_SIDE 2048

/* Draw the SVG in data. want_w/want_h, when positive, ask for a size in
 * pixels; otherwise the drawing's own size is used (small ones doubled up to
 * something that shrinks back sharply). */
static int svg_render(const unsigned char *data, unsigned long len, int want_w,
                      int want_h, image_t *out) {
    out->px = 0;
    out->alpha = 0;
    out->w = out->h = 0;
    /* The parser wants a NUL to stop at, and the buffer it is handed is not
     * guaranteed to have one. */
    char *doc = (char *)malloc(len + 1);
    if (!doc) { img_err = "out of memory"; return 0; }
    memcpy(doc, data, len);
    doc[len] = 0;

    svg_ctx *C = (svg_ctx *)calloc(1, sizeof *C);
    svg_grad *grads = (svg_grad *)calloc(SVG_MAX_GRADS, sizeof *grads);
    if (!C || !grads) { free(C); free(grads); free(doc); img_err = "out of memory"; return 0; }
    C->grads = grads;

    /* --- find the root and work out the size ----------------------------- */
    const char *p = doc;
    const char *root = 0;
    while (*p) {
        if (p[0] == '<' && (p[1] == 's' || p[1] == 'S') &&
            svg_ci_eq(p + 1, "svg", 3) &&
            (svg_isspace(p[4]) || p[4] == '>' || p[4] == '/')) { root = p + 4; break; }
        p++;
    }
    if (!root) { free(grads); free(C); free(doc); img_err = "not an SVG"; return 0; }

    svg_attr rat[SVG_ATTR_MAX];
    const char *rp = root;
    int nrat = svg_attrs(&rp, rat, SVG_ATTR_MAX);

    svg_fx vbx = 0, vby = 0, vbw = 0, vbh = 0;
    const svg_attr *vb = svg_find(rat, nrat, "viewBox");
    if (vb) {
        char b[160];
        svg_attr_str(vb, b, sizeof b);
        const char *q = b;
        vbx = svg_number(&q); vby = svg_number(&q);
        vbw = svg_number(&q); vbh = svg_number(&q);
    }

    svg_fx aw = svg_len(svg_find(rat, nrat, "width"),  vbw ? vbw : 300 * FX_ONE, 0);
    svg_fx ah = svg_len(svg_find(rat, nrat, "height"), vbh ? vbh : 150 * FX_ONE, 0);
    if (aw <= 0) aw = vbw;
    if (ah <= 0) ah = vbh;
    if (aw <= 0) aw = 300 * FX_ONE;
    if (ah <= 0) ah = 150 * FX_ONE;
    if (vbw <= 0 || vbh <= 0) { vbx = vby = 0; vbw = aw; vbh = ah; }

    int W, H;
    if (want_w > 0 && want_h > 0) {
        W = want_w; H = want_h;
    } else {
        W = (aw + FX_HALF) >> 16; H = (ah + FX_HALF) >> 16;
        if (W < SVG_MIN_SIDE) W = SVG_MIN_SIDE;
        if (H < SVG_MIN_SIDE) H = SVG_MIN_SIDE;
        /* Small drawings are rendered larger than asked and shrunk by the
         * caller, which is cheap here and much sharper than scaling up. */
        int up = 1;
        while (W * up < 96 && H * up < 96 && up < 8) up *= 2;
        W *= up; H *= up;
    }
    if (W > SVG_MAX_SIDE) { H = (int)((long)H * SVG_MAX_SIDE / W); W = SVG_MAX_SIDE; }
    if (H > SVG_MAX_SIDE) { W = (int)((long)W * SVG_MAX_SIDE / H); H = SVG_MAX_SIDE; }
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (!img_dims_ok(W, H)) {
        free(grads); free(C); free(doc); img_err = "bad SVG size"; return 0;
    }

    C->px = (unsigned *)calloc((size_t)W * H, sizeof(unsigned));
    if (!C->px) { free(grads); free(C); free(doc); img_err = "out of memory"; return 0; }
    C->w = W; C->h = H;
    C->vbw = vbw; C->vbh = vbh;
    rast_target_init(&C->t, C->px, W, H, W, RAST_ARGB);

    /* viewBox -> pixels, preserving the aspect ratio and centring, which is
     * what the default preserveAspectRatio asks for. */
    svg_fx sx = fx_div((svg_fx)W << 16, vbw);
    svg_fx sy = fx_div((svg_fx)H << 16, vbh);
    const svg_attr *par = svg_find(rat, nrat, "preserveAspectRatio");
    svg_mat root_m = rast_identity();
    if (par && par->vlen >= 4 && svg_ci_eq(par->val, "none", 4)) {
        root_m.a = sx; root_m.d = sy;
        root_m.e = -fx_mul(vbx, sx);
        root_m.f = -fx_mul(vby, sy);
    } else {
        svg_fx s = sx < sy ? sx : sy;
        root_m.a = s; root_m.d = s;
        root_m.e = (((svg_fx)W << 16) - fx_mul(vbw, s)) / 2 - fx_mul(vbx, s);
        root_m.f = (((svg_fx)H << 16) - fx_mul(vbh, s)) / 2 - fx_mul(vby, s);
    }

    svg_collect(C, doc);

    svg_state *s0 = &C->st[0];
    s0->m          = root_m;
    s0->fill       = 0xFF000000u;       /* the initial fill really is black */
    s0->stroke     = 0;
    s0->fill_grad  = s0->stroke_grad = -1;
    s0->color      = 0xFF000000u;
    s0->fill_op    = s0->stroke_op = s0->opacity = 255;
    s0->swidth     = FX_ONE;
    s0->join       = RAST_JOIN_MITER;
    s0->cap        = RAST_CAP_BUTT;
    s0->miter      = 4 * FX_ONE;
    svg_state_apply(C, s0, rat, nrat);

    /* --- walk the tree ---------------------------------------------------- */
    p = rp;
    while (*p && *p != '>') p++;
    if (*p) p++;
    C->depth = 0;
    svg_walk(C, p, doc + len, NULL);

    /* Out as the rest of img.h hands pictures over: laid on the background,
     * with the coverage kept alongside for whoever can use it. */
    long n = (long)W * H;
    unsigned char *alpha = (unsigned char *)malloc((size_t)n);
    int clear = 0;
    unsigned br = (img_background >> 16) & 0xFF, bg = (img_background >> 8) & 0xFF,
             bb = img_background & 0xFF;
    for (long i = 0; i < n; i++) {
        unsigned v = C->px[i], a = v >> 24, inv = 255 - a;
        if (alpha) alpha[i] = (unsigned char)a;
        if (a != 255) clear = 1;
        unsigned r = ((v >> 16) & 0xFF) + (br * inv + 127) / 255;
        unsigned g = ((v >> 8) & 0xFF) + (bg * inv + 127) / 255;
        unsigned b = (v & 0xFF) + (bb * inv + 127) / 255;
        C->px[i] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
    }
    if (!clear) { free(alpha); alpha = 0; }

    out->w = W;
    out->h = H;
    out->px = C->px;
    out->alpha = alpha;
    free(grads);
    free(C);
    free(doc);
    return 1;
}

static int svg_decode(const unsigned char *data, unsigned long len, image_t *out) {
    return svg_render(data, len, 0, 0, out);
}

/* The size the drawing says it is, in pixels -- its width and height, or its
 * viewBox -- without the doubling svg_decode does for sharpness. 0 if it is
 * not an SVG at all. */
static int svg_size(const unsigned char *data, unsigned long len, int *w, int *h) {
    unsigned long lim = len < 4096 ? len : 4096;
    char head[4097];
    memcpy(head, data, lim);
    head[lim] = 0;
    const char *p = head, *root = 0;
    while (*p) {
        if (p[0] == '<' && svg_ci_eq(p + 1, "svg", 3) &&
            (svg_isspace(p[4]) || p[4] == '>' || p[4] == '/')) { root = p + 4; break; }
        p++;
    }
    if (!root) return 0;
    svg_attr rat[SVG_ATTR_MAX];
    int nrat = svg_attrs(&root, rat, SVG_ATTR_MAX);
    svg_fx vbw = 0, vbh = 0;
    const svg_attr *vb = svg_find(rat, nrat, "viewBox");
    if (vb) {
        char b[160];
        svg_attr_str(vb, b, sizeof b);
        const char *q = b;
        svg_number(&q); svg_number(&q);
        vbw = svg_number(&q); vbh = svg_number(&q);
    }
    svg_fx aw = svg_len(svg_find(rat, nrat, "width"),  vbw ? vbw : 300 * FX_ONE, 0);
    svg_fx ah = svg_len(svg_find(rat, nrat, "height"), vbh ? vbh : 150 * FX_ONE, 0);
    /* One side given: the other follows the viewBox's shape. */
    if (aw > 0 && ah <= 0 && vbw > 0) ah = (svg_fx)(((long long)aw * vbh) / vbw);
    if (ah > 0 && aw <= 0 && vbh > 0) aw = (svg_fx)(((long long)ah * vbw) / vbh);
    if (aw <= 0) aw = vbw > 0 ? vbw : 300 * FX_ONE;
    if (ah <= 0) ah = vbh > 0 ? vbh : 150 * FX_ONE;
    *w = (aw + FX_HALF) >> 16;
    *h = (ah + FX_HALF) >> 16;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return 1;
}

#endif /* SVG_H */
