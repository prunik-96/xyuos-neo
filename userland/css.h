#ifndef CSS_H
#define CSS_H

/* The part of CSS this browser can honestly act on.
 *
 * The layout here is a flow of words in character cells: there are no boxes to
 * position, no floats, no grid. So the properties that survive are the ones
 * saying what text IS rather than where it goes -- its colour, its weight, its
 * size, whether it is shown at all -- plus the little spacing the flow can
 * express.
 *
 * The SELECTORS have to be taken seriously, for a reason easy to miss: a rule
 * this engine cannot parse is not a rule that renders plainly, it is a rule
 * that renders WRONG. Refusing "nav a" and keeping "a" would paint every link
 * on the page with the navigation bar's colour. So a selector that is not
 * understood is dropped WHOLE -- and the way to make pages look right is to
 * understand more of them. Hence compound selectors, descendants, children,
 * attribute tests, the structural pseudo-classes, and the media queries that
 * most of a modern stylesheet is nested inside.
 *
 * Deliberately never matched: :hover and the other state pseudo-classes, since
 * we draw the resting page; ::before and ::after, since there is nothing to
 * generate content into. Those are parsed so the selectors beside them in a
 * list survive, and then never apply.
 *
 * Integer only, like everything else that draws.
 */

#include <string.h>
#include <stdlib.h>

#define CSS_MAX_RULES  24000
#define CSS_STR_CAP    (1024 * 1024)
#define CSS_NAME_MAX   40
#define CSS_MAX_CLS    4        /* classes qualifying one compound selector */
#define CSS_MAX_PARTS  4        /* compounds in one selector: "a b c d"      */

enum { CSS_DISP_AUTO = 0, CSS_DISP_NONE, CSS_DISP_BLOCK, CSS_DISP_INLINE,
       CSS_DISP_FLEX, CSS_DISP_GRID };
enum { CSS_FD_AUTO = 0, CSS_FD_ROW, CSS_FD_COLUMN };

/* Whether the element is in the flow at all. A page positions a panel
 * absolutely to lay it over something; read as part of the column, it is a
 * hole in the page the height of whatever it was covering. */
enum { CSS_POS_AUTO = 0, CSS_POS_STATIC, CSS_POS_RELATIVE, CSS_POS_ABSOLUTE,
       CSS_POS_FIXED, CSS_POS_STICKY };
enum { CSS_W_AUTO = 0, CSS_W_NORMAL, CSS_W_BOLD };
enum { CSS_A_AUTO = 0, CSS_A_LEFT, CSS_A_CENTER, CSS_A_RIGHT };
enum { CSS_D_AUTO = 0, CSS_D_NONE, CSS_D_UNDER, CSS_D_STRIKE };

/* Kept apart from display, because a page hides a menu with one and shows it
 * again with the other: `visibility: hidden` on the panel, `visible` on the
 * panel when its container is open. Folded together, the second can never
 * undo the first and the menu stays spilled down the page. */
enum { CSS_V_AUTO = 0, CSS_V_VISIBLE, CSS_V_HIDDEN };
enum { CSS_T_AUTO = 0, CSS_T_NONE, CSS_T_UPPER, CSS_T_LOWER, CSS_T_TITLE };

typedef struct {
    unsigned char display;
    unsigned char weight;
    unsigned char scale;      /* 0 = unset, else 1..3      */
    unsigned char align;
    unsigned char has_color;
    unsigned int  color;
    unsigned char has_bg;
    unsigned int  bg;
    unsigned char decor;      /* text-decoration           */
    unsigned char tcase;      /* text-transform            */
    unsigned char vis;        /* visibility, and opacity: 0 */

    /* How the children of this box stand. A page says `display: flex` six
     * hundred times over and `display: grid` a hundred more; read as a
     * column, every one of those is a row of cards printed one per screen. */
    unsigned char fdir;       /* flex-direction: row or column      */
    unsigned char cols;       /* grid-template-columns: how many, 0 unset */
    /* flex-grow: whether this box wants the room left over in its row, and
     * in what share. Without it every box in a row got the same width, which
     * is right for a row of cards and wrong for everything else -- a menu
     * spread across equal sixths of the page, an article given the same width
     * as the strip beside it. */
    unsigned char grow;
    unsigned char pos;        /* position                           */
    short         gap;        /* between columns, -1 unset          */

    /* The box. Lengths are pixels and -1 means the page said nothing; a width
     * may instead be a percentage of what encloses it, which is why it takes
     * two fields. CSS_LEN_AUTO is a margin the page asked to be shared out,
     * which is how a column gets centred. */
    short         mt, mb, ml, mr;      /* margins  */
    short         pt, pb, pl, pr;      /* padding  */
    short         w_px, w_pct;         /* width    */
    short         maxw_px, maxw_pct;
    /* One line per side, because "border-bottom" is a rule under something
     * and drawing all four for it puts a frame around the page. */
    signed char   bt, bb, bl, br;      /* -1 unset, 0 none, 1 a line */
    unsigned char has_bcolor;
    unsigned int  bcolor;
} css_style_t;

#define CSS_LEN_AUTO (-2)

/* One bit per property, for tracking what a rule set and what it marked
 * !important. */
enum {
    CSSP_DISPLAY = 1 << 0, CSSP_COLOR = 1 << 1, CSSP_WEIGHT = 1 << 2,
    CSSP_SCALE   = 1 << 3, CSSP_ALIGN = 1 << 4, CSSP_MT     = 1 << 5,
    CSSP_MB      = 1 << 6, CSSP_BG    = 1 << 7, CSSP_DECOR  = 1 << 8,
    CSSP_TCASE   = 1 << 9, CSSP_BOX   = 1 << 10, CSSP_WIDTH = 1 << 11,
    CSSP_BORDER  = 1 << 12, CSSP_VIS = 1 << 13, CSSP_FLOW = 1 << 14,
    CSSP_GAP     = 1 << 15, CSSP_POS = 1 << 16, CSSP_GROW = 1 << 17
};

static void css_style_init(css_style_t *s) {
    s->display = CSS_DISP_AUTO;
    s->weight = CSS_W_AUTO;
    s->scale = 0;
    s->align = CSS_A_AUTO;
    s->has_color = 0;
    s->color = 0;
    s->has_bg = 0;
    s->bg = 0;
    s->decor = CSS_D_AUTO;
    s->tcase = CSS_T_AUTO;
    s->vis = CSS_V_AUTO;
    s->fdir = CSS_FD_AUTO;
    s->cols = 0;
    s->grow = 0;
    s->pos = CSS_POS_AUTO;
    s->gap = -1;
    s->mt = s->mb = s->ml = s->mr = -1;
    s->pt = s->pb = s->pl = s->pr = -1;
    s->w_px = s->w_pct = -1;
    s->maxw_px = s->maxw_pct = -1;
    s->bt = s->bb = s->bl = s->br = -1;
    s->has_bcolor = 0;
    s->bcolor = 0;
}

/* --- selectors ------------------------------------------------------------ */

enum { CSS_COMB_DESC = 0, CSS_COMB_CHILD };

enum {
    CSS_PS_NONE = 0,
    CSS_PS_FIRST,      /* :first-child                                */
    CSS_PS_LAST,       /* :last-child                                 */
    CSS_PS_ROOT,       /* :root                                       */
    CSS_PS_NEVER       /* :hover and friends -- parsed, never matched */
};

enum { CSS_AT_NONE = 0, CSS_AT_HAS, CSS_AT_EQ, CSS_AT_PREFIX, CSS_AT_SUFFIX,
       CSS_AT_SUB, CSS_AT_WORD };

/* Every name is an offset into the shared table; 0 means "not given". */
typedef struct {
    int tag, id;
    int cls[CSS_MAX_CLS];
    int attr, aval;
    unsigned char ncls;
    unsigned char pseudo;
    unsigned char aop;
} css_simple;

typedef struct {
    css_simple     part[CSS_MAX_PARTS];      /* the subject is the last one */
    unsigned char  comb[CSS_MAX_PARTS];      /* comb[i] joins part[i] to part[i+1] */
    unsigned char  nparts;
    int            spec;                     /* ids*10000 + classes*100 + tags */
    unsigned int   important;
    css_style_t    style;
} css_rule_t;

static css_rule_t *css_rules;
static int css_nrules;

/* The shared name table. Interned, so a class used a hundred times is stored
 * once and the rules that mention it cost four bytes each. */
static char *css_str;
static int   css_strlen;
#define CSS_STR_BUCKETS 4096
static int   css_shead[CSS_STR_BUCKETS];

static const char *css_name(int off) { return off ? css_str + off : ""; }

/* Every stylesheet the page carries, kept until they can all be read
 * together. */
#define CSS_SRC_CAP (6 * 1024 * 1024)
static char *css_src;
static int   css_srclen;

/* Rules filed by what their subject must be. */
#define CSS_BUCKETS 512
static int  css_bhead[CSS_BUCKETS];      /* first rule in each bucket, -1 */
static int  css_buniv;                   /* rules that match any element  */
static int *css_bnext;                   /* the next rule in its bucket   */

/* What an element looks like to the matcher. The browser builds a chain of
 * these -- the root first, the element itself last -- because a selector may
 * ask about anything above it. */
typedef struct {
    const char   *tag;
    const char   *id;                        /* may be null */
    const char   *cls;                       /* space separated, may be null */
    const char   *attr;                      /* the raw attribute text */
    int           attrlen;
    unsigned char first;                     /* first element child of its parent */
    unsigned char last;
} css_elem;

/* What the media queries are answered against. The browser sets these to the
 * window it actually has. `css_dark` decides prefers-color-scheme, so a site
 * that ships both themes gives us the one that suits the ground we draw on. */
static int css_viewport_w = 1024;
static int css_dark = 1;

/* --- custom properties ---------------------------------------------------- */

#define CSS_VAR_SLOTS  4096
#define CSS_VAR_ARENA  (256 * 1024)

typedef struct { const char *name; const char *val; int nlen, vlen; } css_var;

static css_var  css_vars[CSS_VAR_SLOTS];
static char    *css_var_arena;
static int      css_var_used;
static int      css_var_n;

/* --- small helpers -------------------------------------------------------- */

static int css_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static int css_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Which bucket a name files under. */
static unsigned css_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned)css_lower((unsigned char)*s);
        h *= 16777619u;
    }
    return h & (CSS_BUCKETS - 1);
}

static int css_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        if (css_lower((unsigned char)*a) != css_lower((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

/* Does `word` appear in a space-separated list? That is what a class
 * attribute is. */
static int css_in_list(const char *list, const char *word) {
    if (!list || !word || !*word) return 0;
    int wl = (int)strlen(word);
    for (const char *p = list; *p; ) {
        while (*p && css_space((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !css_space((unsigned char)*p)) p++;
        if (p - s == wl) {
            int i = 0;
            while (i < wl && css_lower((unsigned char)s[i]) == css_lower((unsigned char)word[i])) i++;
            if (i == wl) return 1;
        }
    }
    return 0;
}

static int css_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = css_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const struct { const char *name; unsigned int rgb; } CSS_NAMED[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xCC2222 },
    { "green", 0x188038 }, { "blue", 0x1A56CC }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "navy", 0x203A80 },
    { "teal", 0x117A7A }, { "olive", 0x7A7A11 }, { "maroon", 0x8A2020 },
    { "purple", 0x7A2C9E }, { "yellow", 0xB89000 }, { "orange", 0xD1741A },
    { "lime", 0x2FA02F }, { "aqua", 0x1A9EA0 }, { "fuchsia", 0xB030B0 },
    { "darkred", 0x8A1F1F }, { "crimson", 0xB0223C },
    { "pink", 0xE08AA8 }, { "brown", 0x8A5A2B }, { "gold", 0xC0A020 },
    { "cyan", 0x1A9EA0 }, { "magenta", 0xB030B0 }, { "indigo", 0x4B0082 },
    { "violet", 0x9040C0 }, { "salmon", 0xD07060 }, { "khaki", 0xB0A060 },
    { "beige", 0xE8E4D0 }, { "ivory", 0xFFFFF0 }, { "tan", 0xC0A080 },
    { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "lightblue", 0x9AC8E0 }, { "darkblue", 0x1A2A80 },
    { "lightgreen", 0x8AD08A }, { "darkgreen", 0x0F5A28 },
};

/* Returns 1 and sets *out. hsl(), colour functions and custom properties are
 * left alone rather than guessed at. */
static int css_color(const char *v, int len, unsigned int *out) {
    while (len > 0 && css_space((unsigned char)*v)) { v++; len--; }
    if (len <= 0) return 0;

    if (*v == '#') {
        v++; len--;
        int n = 0;
        while (n < len && css_hex((unsigned char)v[n]) >= 0) n++;
        if (n >= 6) {
            int r = css_hex(v[0]) * 16 + css_hex(v[1]);
            int g = css_hex(v[2]) * 16 + css_hex(v[3]);
            int b = css_hex(v[4]) * 16 + css_hex(v[5]);
            *out = (unsigned)((r << 16) | (g << 8) | b);
            return 1;
        }
        if (n == 3) {
            int r = css_hex(v[0]), g = css_hex(v[1]), b = css_hex(v[2]);
            *out = (unsigned)(((r * 17) << 16) | ((g * 17) << 8) | (b * 17));
            return 1;
        }
        return 0;
    }

    if (len > 4 && css_lower((unsigned char)v[0]) == 'r' &&
                   css_lower((unsigned char)v[1]) == 'g' &&
                   css_lower((unsigned char)v[2]) == 'b') {
        int c[3] = { 0, 0, 0 }, k = 0, i = 3;
        while (i < len && v[i] != '(') i++;
        i++;
        while (i < len && k < 3) {
            if (v[i] >= '0' && v[i] <= '9') {
                int val = 0;
                while (i < len && v[i] >= '0' && v[i] <= '9') val = val * 10 + (v[i++] - '0');
                c[k++] = val > 255 ? 255 : val;
            } else if (v[i] == ')') {
                break;
            } else {
                i++;
            }
        }
        if (k < 3) return 0;
        *out = (unsigned)((c[0] << 16) | (c[1] << 8) | c[2]);
        return 1;
    }

    char name[24];
    int n = 0;
    while (n < len && n < (int)sizeof name - 1 && !css_space((unsigned char)v[n]) &&
           v[n] != ';' && v[n] != '}') { name[n] = v[n]; n++; }
    name[n] = 0;
    for (unsigned i = 0; i < sizeof CSS_NAMED / sizeof CSS_NAMED[0]; i++)
        if (css_ieq(name, CSS_NAMED[i].name)) { *out = CSS_NAMED[i].rgb; return 1; }
    return 0;
}

/* A number in hundredths, plus where its unit starts. Integer only, because
 * these programs build without floating point. */
static int css_num100(const char *v, int len, int *unit) {
    int i = 0, neg = 0;
    while (i < len && css_space((unsigned char)v[i])) i++;
    if (i < len && (v[i] == '-' || v[i] == '+')) { neg = (v[i] == '-'); i++; }
    long whole = 0, frac = 0, fdiv = 1;
    int digits = 0;
    while (i < len && v[i] >= '0' && v[i] <= '9') { whole = whole * 10 + (v[i++] - '0'); digits++; }
    if (i < len && v[i] == '.') {
        i++;
        while (i < len && v[i] >= '0' && v[i] <= '9' && fdiv < 100) {
            frac = frac * 10 + (v[i++] - '0');
            fdiv *= 10;
            digits++;
        }
        while (i < len && v[i] >= '0' && v[i] <= '9') i++;
    }
    if (!digits) { *unit = i; return 0; }
    long n = whole * 100 + (fdiv > 1 ? frac * 100 / fdiv : 0);
    *unit = i;
    return (int)(neg ? -n : n);
}

/* A font size reduced to one of the sizes the face can draw. The font is
 * monospace and scaled by whole numbers, so there are three. */
static int css_font_scale(const char *v, int len) {
    while (len > 0 && css_space((unsigned char)*v)) { v++; len--; }
    if (len <= 0) return 0;

    static const struct { const char *k; int s; } WORDS[] = {
        { "xx-small", 1 }, { "x-small", 1 }, { "small", 1 }, { "smaller", 1 },
        { "medium", 1 }, { "large", 2 }, { "larger", 2 },
        { "x-large", 2 }, { "xx-large", 3 },
    };
    for (unsigned i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        int kl = (int)strlen(WORDS[i].k);
        if (len >= kl) {
            int j = 0;
            while (j < kl && css_lower((unsigned char)v[j]) == WORDS[i].k[j]) j++;
            if (j == kl) return WORDS[i].s;
        }
    }

    int unit;
    int n = css_num100(v, len, &unit);
    if (n <= 0) return 0;
    const char *u = v + unit;
    int ul = len - unit;

    int px100;                                  /* size in hundredths of a px */
    if (ul >= 2 && css_lower((unsigned char)u[0]) == 'e' && css_lower((unsigned char)u[1]) == 'm')
        px100 = n * 16;
    else if (ul >= 3 && css_lower((unsigned char)u[0]) == 'r')
        px100 = n * 16;
    else if (ul >= 1 && u[0] == '%')
        px100 = n * 16 / 100;
    else
        px100 = n;                              /* px, pt and the rest: close enough */

    if (px100 >= 3400) return 3;
    if (px100 >= 2100) return 2;
    return 1;
}

/* A length that may be given as a share of what encloses it. Returns 1 if it
 * was a percentage, and puts the number in *pct rather than *px. */
static int css_length_or_pct(const char *v, int len, int *px, int *pct) {
    int unit;
    int n = css_num100(v, len, &unit);
    const char *u = v + unit;
    int ul = len - unit;
    while (ul > 0 && css_space((unsigned char)*u)) { u++; ul--; }

    if (ul >= 1 && u[0] == '%') { *pct = n / 100; return 1; }
    if (ul >= 4 && css_lower((unsigned char)u[0]) == 'a') {   /* auto */
        *px = CSS_LEN_AUTO; return 0;
    }
    if (ul >= 2 && css_lower((unsigned char)u[0]) == 'e' &&
                   css_lower((unsigned char)u[1]) == 'm') { *px = n * 16 / 100; return 0; }
    if (ul >= 3 && css_lower((unsigned char)u[0]) == 'r') { *px = n * 16 / 100; return 0; }
    if (ul >= 2 && css_lower((unsigned char)u[0]) == 'v') {
        /* vw and vh: a share of the window, near enough for a column width. */
        *pct = n / 100;
        return 1;
    }
    *px = n / 100;
    return 0;
}

/* A length, in pixels. */
static int css_length_px(const char *v, int len) {
    int unit;
    int n = css_num100(v, len, &unit);
    const char *u = v + unit;
    int ul = len - unit;
    if (ul >= 2 && css_lower((unsigned char)u[0]) == 'e' && css_lower((unsigned char)u[1]) == 'm')
        return n * 16 / 100;
    if (ul >= 3 && css_lower((unsigned char)u[0]) == 'r')
        return n * 16 / 100;
    return n / 100;
}

/* Compare a value token against a word, without copying it out. */
static int css_vword(const char *v, int vl, const char *w) {
    int i = 0;
    while (i < vl && w[i] && css_lower((unsigned char)v[i]) == (unsigned char)w[i]) i++;
    return !w[i] && (i == vl || css_space((unsigned char)v[i]) || v[i] == ',');
}

/* How many columns a grid-template-columns asks for. Zero means "however many
 * fit", which is what repeat(auto-fit, ...) says and what we do regardless. */
static int css_track_count(const char *v, int vl) {
    int n = 0, i = 0;
    while (i < vl) {
        while (i < vl && css_space((unsigned char)v[i])) i++;
        if (i >= vl) break;
        int start = i, depth = 0;
        while (i < vl && (depth || !css_space((unsigned char)v[i]))) {
            if (v[i] == '(') depth++;
            else if (v[i] == ')') depth--;
            i++;
        }
        const char *t = v + start;
        int tl = i - start;
        if (css_vword(t, tl, "none")) return 0;
        int isrep = 0;
        if (tl > 7) {
            static const char rep[] = "repeat";
            int m = 0;
            while (m < 6 && css_lower((unsigned char)t[m]) == (unsigned char)rep[m]) m++;
            isrep = (m == 6 && t[6] == '(');
        }
        if (isrep) {
            /* repeat(N, ...) is N tracks; repeat(auto-fit, ...) is a count
             * the page is leaving to the browser, which is to say to us. */
            int j = 7, k = 0, got = 0;
            while (j < tl && t[j] >= '0' && t[j] <= '9') {
                k = k * 10 + (t[j] - '0'); j++; got = 1;
            }
            if (!got) return 0;
            n += k;
        } else {
            n++;
        }
    }
    return n > 64 ? 64 : n;
}

/* --- custom properties ----------------------------------------------------
 *
 * Open addressing, because there are two thousand of them per theme and the
 * lookup happens for every value that mentions one. */

static unsigned css_vhash(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h ^= (unsigned)css_lower((unsigned char)s[i]);
        h *= 16777619u;
    }
    return h;
}

static int css_var_eq(const css_var *v, const char *n, int nl) {
    if (v->nlen != nl) return 0;
    for (int i = 0; i < nl; i++)
        if (css_lower((unsigned char)v->name[i]) != css_lower((unsigned char)n[i]))
            return 0;
    return 1;
}

static void css_var_set(const char *n, int nl, const char *v, int vl) {
    while (vl > 0 && css_space((unsigned char)*v)) { v++; vl--; }
    while (vl > 0 && css_space((unsigned char)v[vl - 1])) vl--;
    if (nl <= 0 || nl > 120 || vl < 0 || vl > 400) return;
    if (css_var_used + nl + vl + 2 > CSS_VAR_ARENA) return;

    unsigned h = css_vhash(n, nl);
    for (int probe = 0; probe < CSS_VAR_SLOTS; probe++) {
        int i = (int)((h + (unsigned)probe) & (CSS_VAR_SLOTS - 1));
        if (!css_vars[i].name) {
            if (css_var_n >= CSS_VAR_SLOTS * 3 / 4) return;
            char *pn = css_var_arena + css_var_used;
            memcpy(pn, n, (size_t)nl); pn[nl] = 0;
            char *pv = pn + nl + 1;
            memcpy(pv, v, (size_t)vl); pv[vl] = 0;
            css_var_used += nl + vl + 2;
            css_vars[i].name = pn; css_vars[i].nlen = nl;
            css_vars[i].val = pv;  css_vars[i].vlen = vl;
            css_var_n++;
            return;
        }
        if (css_var_eq(&css_vars[i], n, nl)) {
            /* A later definition replaces an earlier one. */
            char *pv = css_var_arena + css_var_used;
            memcpy(pv, v, (size_t)vl); pv[vl] = 0;
            css_var_used += vl + 1;
            css_vars[i].val = pv; css_vars[i].vlen = vl;
            return;
        }
    }
}

static const char *css_var_get(const char *n, int nl, int *vlen) {
    unsigned h = css_vhash(n, nl);
    for (int probe = 0; probe < CSS_VAR_SLOTS; probe++) {
        int i = (int)((h + (unsigned)probe) & (CSS_VAR_SLOTS - 1));
        if (!css_vars[i].name) return 0;
        if (css_var_eq(&css_vars[i], n, nl)) { *vlen = css_vars[i].vlen; return css_vars[i].val; }
    }
    return 0;
}

/* Replace every var(--name) or var(--name, fallback) in a value. Variables
 * may name other variables, so this runs until it settles or gives up. */
static int css_expand(const char *v, int vl, char *out, int max) {
    int n = 0, i = 0;
    while (i < vl && n < max - 1) {
        if (i + 4 <= vl && (v[i] == 'v' || v[i] == 'V') &&
            css_lower((unsigned char)v[i+1]) == 'a' &&
            css_lower((unsigned char)v[i+2]) == 'r' && v[i+3] == '(') {
            int j = i + 4, depth = 1, comma = -1;
            while (j < vl && depth) {
                if (v[j] == '(') depth++;
                else if (v[j] == ')') { depth--; if (!depth) break; }
                else if (v[j] == ',' && depth == 1 && comma < 0) comma = j;
                j++;
            }
            int ns = i + 4, ne = (comma >= 0) ? comma : j;
            while (ns < ne && css_space((unsigned char)v[ns])) ns++;
            while (ne > ns && css_space((unsigned char)v[ne - 1])) ne--;

            int rl = 0;
            const char *rv = css_var_get(v + ns, ne - ns, &rl);
            if (!rv && comma >= 0) { rv = v + comma + 1; rl = j - comma - 1; }
            if (rv) {
                for (int k = 0; k < rl && n < max - 1; k++) out[n++] = rv[k];
            }
            i = (j < vl) ? j + 1 : vl;
            continue;
        }
        out[n++] = v[i++];
    }
    out[n] = 0;
    return n;
}

static int css_isdigit_x(int c) { return c >= '0' && c <= '9'; }

/* --- declarations --------------------------------------------------------- */

/* One "name: value" pair into a style. Records which property it set, and
 * separately whether that one was marked !important. */
static void css_decl(css_style_t *st, unsigned int *touched,
                     unsigned int *important,
                     const char *n, int nl, const char *v, int vl) {
    char name[32];
    int k = 0;
    for (int i = 0; i < nl && k < (int)sizeof name - 1; i++) {
        if (css_space((unsigned char)n[i])) continue;
        name[k++] = (char)css_lower((unsigned char)n[i]);
    }
    name[k] = 0;
    while (vl > 0 && css_space((unsigned char)v[vl - 1])) vl--;
    while (vl > 0 && css_space((unsigned char)*v)) { v++; vl--; }
    if (vl <= 0) return;

    int imp = 0;
    for (int i = 0; i + 1 < vl; i++)
        if (v[i] == '!') { imp = 1; vl = i; break; }
    while (vl > 0 && css_space((unsigned char)v[vl - 1])) vl--;
    if (vl <= 0) return;

    /* A value that names a custom property is worth nothing until the name is
     * replaced by what it stands for. On a modern page that is most of them. */
    char expanded[512];
    for (int i = 0; i + 3 < vl; i++) {
        if ((v[i] == 'v' || v[i] == 'V') && css_lower((unsigned char)v[i+1]) == 'a' &&
            css_lower((unsigned char)v[i+2]) == 'r' && v[i+3] == '(') {
            int pass = 0;
            int n2 = css_expand(v, vl, expanded, sizeof expanded);
            /* A variable may stand for another variable. */
            while (pass++ < 4) {
                int has = 0;
                for (int k = 0; k + 3 < n2; k++)
                    if (expanded[k] == 'v' && expanded[k+1] == 'a' &&
                        expanded[k+2] == 'r' && expanded[k+3] == '(') { has = 1; break; }
                if (!has) break;
                char again[512];
                int n3 = css_expand(expanded, n2, again, sizeof again);
                memcpy(expanded, again, (size_t)n3 + 1);
                n2 = n3;
            }
            v = expanded;
            vl = n2;
            while (vl > 0 && css_space((unsigned char)v[vl - 1])) vl--;
            while (vl > 0 && css_space((unsigned char)*v)) { v++; vl--; }
            break;
        }
    }
    if (vl <= 0) return;

    unsigned int bit = 0;

    if (css_ieq(name, "display")) {
        /* inline-flex and inline-grid arrange their children exactly as the
         * block kinds do; all that differs is the box around them, and we
         * have no such box. inline-block stays inline, because making it a
         * block would put every button in a row on a line of its own. */
        if (css_vword(v, vl, "none"))                                st->display = CSS_DISP_NONE;
        else if (css_vword(v, vl, "flex") || css_vword(v, vl, "inline-flex"))
            st->display = CSS_DISP_FLEX;
        else if (css_vword(v, vl, "grid") || css_vword(v, vl, "inline-grid"))
            st->display = CSS_DISP_GRID;
        else if (css_vword(v, vl, "inline") || css_vword(v, vl, "inline-block"))
            st->display = CSS_DISP_INLINE;
        else if (css_vword(v, vl, "block"))                          st->display = CSS_DISP_BLOCK;
        bit = CSSP_DISPLAY;
    } else if (css_ieq(name, "flex-direction") || css_ieq(name, "flex-flow")) {
        /* The shorthand carries a wrap word too, which we do not need: we
         * wrap whenever the columns would otherwise be too narrow to read. */
        if (css_vword(v, vl, "column") || css_vword(v, vl, "column-reverse"))
            st->fdir = CSS_FD_COLUMN;
        else
            st->fdir = CSS_FD_ROW;
        bit = CSSP_FLOW;
    } else if (css_ieq(name, "flex-grow") || css_ieq(name, "flex")) {
        /* The shorthand's first number is the grow factor; `flex: auto` and
         * `flex: 1 1 auto` both come to one, and `flex: none` to nothing. */
        if (css_vword(v, vl, "none")) st->grow = 0;
        else if (css_vword(v, vl, "auto") || css_vword(v, vl, "initial")) st->grow = 1;
        else {
            int unit, g = css_num100(v, vl, &unit);
            if (g < 0) g = 0;
            g /= 100;
            st->grow = (unsigned char)(g > 32 ? 32 : g);
        }
        bit = CSSP_GROW;
    } else if (css_ieq(name, "position")) {
        if      (css_vword(v, vl, "absolute")) st->pos = CSS_POS_ABSOLUTE;
        else if (css_vword(v, vl, "fixed"))    st->pos = CSS_POS_FIXED;
        else if (css_vword(v, vl, "sticky"))   st->pos = CSS_POS_STICKY;
        else if (css_vword(v, vl, "relative")) st->pos = CSS_POS_RELATIVE;
        else if (css_vword(v, vl, "static"))   st->pos = CSS_POS_STATIC;
        bit = CSSP_POS;
    } else if (css_ieq(name, "grid-template-columns")) {
        int n = css_track_count(v, vl);
        if (n > 0) { st->cols = (unsigned char)n; bit = CSSP_FLOW; }
    } else if (css_ieq(name, "gap") || css_ieq(name, "grid-gap") ||
               css_ieq(name, "column-gap") || css_ieq(name, "grid-column-gap")) {
        int px = css_length_px(v, vl);
        if (px >= 0) { st->gap = (short)(px > 200 ? 200 : px); bit = CSSP_GAP; }
    } else if (css_ieq(name, "visibility")) {
        int c = css_lower((unsigned char)v[0]);
        if (c == 'h') { st->vis = CSS_V_HIDDEN; bit = CSSP_VIS; }
        else if (c == 'v') { st->vis = CSS_V_VISIBLE; bit = CSSP_VIS; }
    } else if (css_ieq(name, "opacity")) {
        /* Fully transparent is the only opacity a surface without an alpha
         * channel can express -- and it is the one pages use to hide things. */
        int unit, o = css_num100(v, vl, &unit);
        st->vis = (o == 0) ? CSS_V_HIDDEN : CSS_V_VISIBLE;
        bit = CSSP_VIS;
    } else if (css_ieq(name, "color")) {
        unsigned int c;
        if (css_color(v, vl, &c)) { st->color = c; st->has_color = 1; bit = CSSP_COLOR; }
    } else if (css_ieq(name, "background-color") || css_ieq(name, "background")) {
        /* The shorthand carries images, positions and repeats as well; the
         * colour is the only part of it there is anything to draw. */
        unsigned int c;
        if (css_lower((unsigned char)v[0]) == 't') {          /* transparent */
            bit = CSSP_BG;
        } else if (css_color(v, vl, &c)) {
            st->bg = c; st->has_bg = 1; bit = CSSP_BG;
        }
    } else if (css_ieq(name, "font-weight")) {
        if (v[0] >= '0' && v[0] <= '9') {
            int unit, n2 = css_num100(v, vl, &unit);
            st->weight = (n2 >= 60000) ? CSS_W_BOLD : CSS_W_NORMAL;
        } else if (css_lower((unsigned char)v[0]) == 'b') {
            st->weight = CSS_W_BOLD;
        } else if (css_lower((unsigned char)v[0]) == 'n' || css_lower((unsigned char)v[0]) == 'l') {
            st->weight = CSS_W_NORMAL;
        }
        bit = CSSP_WEIGHT;
    } else if (css_ieq(name, "font-size")) {
        int sc = css_font_scale(v, vl);
        if (sc) { st->scale = (unsigned char)sc; bit = CSSP_SCALE; }
    } else if (css_ieq(name, "text-align")) {
        int c = css_lower((unsigned char)v[0]);
        if (c == 'c') st->align = CSS_A_CENTER;
        else if (c == 'r') st->align = CSS_A_RIGHT;
        else if (c == 'l' || c == 's') st->align = CSS_A_LEFT;
        bit = CSSP_ALIGN;
    } else if (css_ieq(name, "text-decoration") ||
               css_ieq(name, "text-decoration-line")) {
        int c = css_lower((unsigned char)v[0]);
        if (c == 'n') st->decor = CSS_D_NONE;
        else if (c == 'u') st->decor = CSS_D_UNDER;
        else if (c == 'l') st->decor = CSS_D_STRIKE;          /* line-through */
        bit = CSSP_DECOR;
    } else if (css_ieq(name, "text-transform")) {
        int c = css_lower((unsigned char)v[0]);
        if (c == 'u') st->tcase = CSS_T_UPPER;
        else if (c == 'l') st->tcase = CSS_T_LOWER;
        else if (c == 'c') st->tcase = CSS_T_TITLE;
        else if (c == 'n') st->tcase = CSS_T_NONE;
        bit = CSSP_TCASE;
    } else if (css_ieq(name, "margin-top")) {
        st->mt = (short)css_length_px(v, vl); bit = CSSP_MT;
    } else if (css_ieq(name, "margin-bottom")) {
        st->mb = (short)css_length_px(v, vl); bit = CSSP_MB;
    } else if (css_ieq(name, "margin-left") || css_ieq(name, "margin-right")) {
        int px = -1, pct = -1;
        css_length_or_pct(v, vl, &px, &pct);
        if (pct >= 0) px = 0;                 /* a side margin in percent is
                                                 spacing, not a column */
        if (css_ieq(name, "margin-left")) st->ml = (short)px; else st->mr = (short)px;
        bit = CSSP_BOX;
    } else if (css_ieq(name, "padding-top") || css_ieq(name, "padding-bottom") ||
               css_ieq(name, "padding-left") || css_ieq(name, "padding-right")) {
        int px = css_length_px(v, vl);
        if (px < 0) px = 0;
        if (px > 200) px = 200;
        char side = name[8];
        if (side == 't') st->pt = (short)px;
        else if (side == 'b') st->pb = (short)px;
        else if (side == 'l') st->pl = (short)px;
        else st->pr = (short)px;
        bit = CSSP_BOX;
    } else if (css_ieq(name, "padding")) {
        int val[4] = { 0, 0, 0, 0 }, got = 0, i = 0;
        while (i < vl && got < 4) {
            while (i < vl && css_space((unsigned char)v[i])) i++;
            int s2 = i;
            while (i < vl && !css_space((unsigned char)v[i])) i++;
            if (i > s2) {
                int px = css_length_px(v + s2, i - s2);
                val[got++] = px < 0 ? 0 : (px > 200 ? 200 : px);
            }
        }
        if (got) {
            st->pt = (short)val[0];
            st->pr = (short)(got >= 2 ? val[1] : val[0]);
            st->pb = (short)(got >= 3 ? val[2] : val[0]);
            st->pl = (short)(got >= 4 ? val[3] : (got >= 2 ? val[1] : val[0]));
            bit = CSSP_BOX;
        }
    } else if (css_ieq(name, "width")) {
        int px = -1, pct = -1;
        css_length_or_pct(v, vl, &px, &pct);
        st->w_px = (short)px; st->w_pct = (short)pct;
        bit = CSSP_WIDTH;
    } else if (css_ieq(name, "max-width")) {
        int px = -1, pct = -1;
        css_length_or_pct(v, vl, &px, &pct);
        st->maxw_px = (short)px; st->maxw_pct = (short)pct;
        bit = CSSP_WIDTH;
    } else if (css_ieq(name, "border-color")) {
        /* A colour on its own does not make a border; it says what colour the
         * border would be if there were one. */
        unsigned int c;
        if (css_color(v, vl, &c)) { st->bcolor = c; st->has_bcolor = 1; bit = CSSP_BORDER; }
    } else if (!strncmp(name, "border", 6)) {
        /* Which sides, and whether there is anything on them. Any width at
         * all becomes one line: at this size a three pixel ridge and a hair
         * line carry the same information. */
        int t = 1, b2 = 1, l = 1, r = 1;
        const char *rest = name + 6;
        if (*rest == '-') {
            rest++;
            if (!strncmp(rest, "top", 3))         { b2 = l = r = 0; }
            else if (!strncmp(rest, "bottom", 6)) { t = l = r = 0; }
            else if (!strncmp(rest, "left", 4))   { t = b2 = r = 0; }
            else if (!strncmp(rest, "right", 5))  { t = b2 = l = 0; }
            else if (strncmp(rest, "width", 5) && strncmp(rest, "style", 5)) {
                /* border-radius, border-collapse, border-image and the rest
                 * are not lines and are left alone. */
                rest = 0;
            }
        }
        if (rest) {
            unsigned int c;
            if (css_color(v, vl, &c)) { st->bcolor = c; st->has_bcolor = 1; }
            int on = 1;
            int c0 = css_lower((unsigned char)v[0]);
            if (c0 == 'n' || (c0 == '0' && (vl == 1 || !css_isdigit_x(v[1])))) on = 0;
            if (t)  st->bt = (signed char)on;
            if (b2) st->bb = (signed char)on;
            if (l)  st->bl = (signed char)on;
            if (r)  st->br = (signed char)on;
            bit = CSSP_BORDER;
        }
    } else if (css_ieq(name, "margin")) {
        /* One to four values: the first is the top, and the third -- or the
         * first again -- is the bottom. */
        int i = 0, got = 0, val[4] = { 0, 0, 0, 0 };
        while (i < vl && got < 4) {
            while (i < vl && css_space((unsigned char)v[i])) i++;
            int s2 = i;
            while (i < vl && !css_space((unsigned char)v[i])) i++;
            if (i > s2) {
                int px = -1, pct = -1;
                css_length_or_pct(v + s2, i - s2, &px, &pct);
                val[got++] = (pct >= 0) ? 0 : px;
            }
        }
        if (got) {
            st->mt = (short)(val[0] == CSS_LEN_AUTO ? 0 : val[0]);
            st->mb = (short)((got >= 3 ? val[2] : val[0]) == CSS_LEN_AUTO
                             ? 0 : (got >= 3 ? val[2] : val[0]));
            /* The left and right of the shorthand are what centres a column. */
            int side = (got >= 2) ? val[1] : val[0];
            st->mr = (short)side;
            st->ml = (short)((got >= 4) ? val[3] : side);
            bit = CSSP_MT | CSSP_MB | CSSP_BOX;
        }
    }

    if (bit) {
        *touched |= bit;
        if (imp) *important |= bit;
    }
}

/* A whole declaration block, "a: b; c: d". */
static void css_decls_full(css_style_t *st, unsigned int *touched,
                           unsigned int *important, const char *s, int len) {
    int i = 0;
    while (i < len) {
        int ns = i;
        while (i < len && s[i] != ':' && s[i] != ';' && s[i] != '}') i++;
        if (i >= len || s[i] != ':') {          /* junk, skip to the next one */
            while (i < len && s[i] != ';') i++;
            i++;
            continue;
        }
        int ne = i++;
        int vs = i;
        int depth = 0;
        while (i < len && (depth || (s[i] != ';' && s[i] != '}'))) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')' && depth) depth--;
            i++;
        }
        css_decl(st, touched, important, s + ns, ne - ns, s + vs, i - vs);
        i++;
    }
}

/* What a style attribute needs: the values, without the bookkeeping. */
static void css_decls(css_style_t *st, const char *s, int len) {
    unsigned int t = 0, imp = 0;
    css_decls_full(st, &t, &imp, s, len);
}

/* --- stylesheets ---------------------------------------------------------- */

static void css_reset(void) {
    css_nrules = 0;
    css_buniv = -1;
    for (int i = 0; i < CSS_BUCKETS; i++) css_bhead[i] = -1;
    css_srclen = 0;
    css_strlen = 4;                    /* offset 0 is reserved for "none" */
    for (int i = 0; i < CSS_STR_BUCKETS; i++) css_shead[i] = 0;
    css_var_used = 0;
    css_var_n = 0;
    for (int i = 0; i < CSS_VAR_SLOTS; i++) css_vars[i].name = 0;
}

static int css_init(void) {
    if (!css_rules)
        css_rules = (css_rule_t *)malloc((size_t)CSS_MAX_RULES * sizeof(css_rule_t));
    if (!css_bnext)
        css_bnext = (int *)malloc((size_t)CSS_MAX_RULES * sizeof(int));
    if (!css_src)       css_src = (char *)malloc(CSS_SRC_CAP);
    if (!css_str)       css_str = (char *)malloc(CSS_STR_CAP);
    if (!css_var_arena) css_var_arena = (char *)malloc(CSS_VAR_ARENA);
    css_reset();
    return css_rules && css_bnext && css_src && css_str && css_var_arena;
}

/* Put a name in the shared table, or find it if it is already there. Each
 * entry is a four byte link to the next in its bucket, then the characters. */
static int css_intern(const char *n, int len) {
    if (len <= 0 || !css_str) return 0;
    unsigned h = css_vhash(n, len) & (CSS_STR_BUCKETS - 1);
    for (int off = css_shead[h]; off; ) {
        const char *e = css_str + off;
        int i = 0;
        while (i < len && e[i] && e[i] == (char)css_lower((unsigned char)n[i])) i++;
        if (i == len && !e[i]) return off;
        int nxt;
        memcpy(&nxt, css_str + off - 4, sizeof nxt);
        off = nxt;
    }
    if (css_strlen + len + 8 > CSS_STR_CAP) return 0;
    int link = css_strlen;
    int prev = css_shead[h];
    memcpy(css_str + link, &prev, sizeof prev);
    int off = link + 4;
    for (int i = 0; i < len; i++) css_str[off + i] = (char)css_lower((unsigned char)n[i]);
    css_str[off + len] = 0;
    css_strlen = off + len + 1;
    css_shead[h] = off;
    return off;
}

/* Copy an identifier, lowercased. Returns how many characters it took. */
static int css_ident(const char *s, int len, char *out, int max) {
    int n = 0;
    while (n < len && n < max - 1) {
        char c = s[n];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            (unsigned char)c >= 0x80)
            out[n] = (char)css_lower((unsigned char)c);
        else break;
        n++;
    }
    out[n] = 0;
    return n;
}

/* One compound selector: a tag, and any number of #id, .class, [attr] and
 * :pseudo qualifiers on it. Returns how much it consumed, or 0 on something
 * unparseable. */
static int css_parse_simple(const char *s, int len, css_simple *out, int *spec) {
    memset(out, 0, sizeof *out);
    int i = 0;

    char nm[CSS_NAME_MAX];
    if (i < len && s[i] == '*') {
        i++;                                   /* any element, no specificity */
    } else if (i < len && (s[i] == '.' || s[i] == '#' || s[i] == '[' || s[i] == ':')) {
        /* qualifiers with no tag in front: still any element */
    } else {
        int n = css_ident(s + i, len - i, nm, CSS_NAME_MAX);
        if (!n) return 0;
        out->tag = css_intern(nm, n);
        i += n;
        *spec += 1;
    }

    while (i < len) {
        char c = s[i];
        if (c == '#') {
            i++;
            int n = css_ident(s + i, len - i, nm, CSS_NAME_MAX);
            if (!n) return 0;
            out->id = css_intern(nm, n);
            i += n;
            *spec += 10000;
        } else if (c == '.') {
            i++;
            if (out->ncls >= CSS_MAX_CLS) return 0;
            int n = css_ident(s + i, len - i, nm, CSS_NAME_MAX);
            if (!n) return 0;
            out->cls[out->ncls++] = css_intern(nm, n);
            i += n;
            *spec += 100;
        } else if (c == '[') {
            i++;
            while (i < len && css_space((unsigned char)s[i])) i++;
            int n = css_ident(s + i, len - i, nm, CSS_NAME_MAX);
            if (!n) return 0;
            out->attr = css_intern(nm, n);
            i += n;
            while (i < len && css_space((unsigned char)s[i])) i++;
            if (i < len && s[i] == ']') { out->aop = CSS_AT_HAS; i++; }
            else {
                int op = CSS_AT_EQ;
                if (i < len && (s[i] == '^' || s[i] == '$' || s[i] == '*' || s[i] == '~')) {
                    op = (s[i] == '^') ? CSS_AT_PREFIX : (s[i] == '$') ? CSS_AT_SUFFIX
                       : (s[i] == '*') ? CSS_AT_SUB : CSS_AT_WORD;
                    i++;
                }
                if (i >= len || s[i] != '=') return 0;
                i++;
                while (i < len && css_space((unsigned char)s[i])) i++;
                char q = 0;
                if (i < len && (s[i] == '"' || s[i] == '\'')) q = s[i++];
                char val[CSS_NAME_MAX];
                int k = 0;
                while (i < len && k < CSS_NAME_MAX - 1) {
                    if (q ? s[i] == q : (s[i] == ']' || css_space((unsigned char)s[i]))) break;
                    val[k++] = s[i++];
                }
                val[k] = 0;
                out->aval = css_intern(val, k);
                if (q && i < len && s[i] == q) i++;
                while (i < len && s[i] != ']') i++;
                if (i < len) i++;
                out->aop = (unsigned char)op;
            }
            *spec += 100;
        } else if (c == ':') {
            i++;
            int dbl = 0;
            if (i < len && s[i] == ':') { i++; dbl = 1; }
            char ps[CSS_NAME_MAX];
            int n = css_ident(s + i, len - i, ps, CSS_NAME_MAX);
            if (!n) return 0;
            i += n;

            if (i < len && s[i] == '(') {
                /* A functional pseudo-class takes an argument we do not
                 * evaluate; step over it so the rest still parses. */
                int depth = 1;
                i++;
                while (i < len && depth) {
                    if (s[i] == '(') depth++;
                    else if (s[i] == ')') depth--;
                    i++;
                }
                out->pseudo = CSS_PS_NEVER;
            } else if (dbl) {
                out->pseudo = CSS_PS_NEVER;                  /* ::before/::after */
            } else if (css_ieq(ps, "first-child") || css_ieq(ps, "first-of-type")) {
                out->pseudo = CSS_PS_FIRST;
            } else if (css_ieq(ps, "last-child") || css_ieq(ps, "last-of-type")) {
                out->pseudo = CSS_PS_LAST;
            } else if (css_ieq(ps, "root")) {
                out->pseudo = CSS_PS_ROOT;
            } else {
                /* :hover, :focus, :active, :visited, :checked -- states this
                 * browser does not have. Never matched, which is the page at
                 * rest, and that is what we draw. */
                out->pseudo = CSS_PS_NEVER;
            }
            *spec += 100;
        } else {
            break;
        }
    }
    return i;
}

/* One selector out of a comma-separated list. Returns 0 if it is something
 * this engine will not pretend to understand -- and dropping it whole is the
 * point, since half a selector paints the wrong things. */
static int css_add_selector(const char *sel, int len, const css_style_t *st,
                            unsigned int important) {
    while (len > 0 && css_space((unsigned char)*sel)) { sel++; len--; }
    while (len > 0 && css_space((unsigned char)sel[len - 1])) len--;
    if (len <= 0 || css_nrules >= CSS_MAX_RULES) return 0;

    css_rule_t *r = &css_rules[css_nrules];
    memset(r, 0, sizeof *r);

    int i = 0, comb = CSS_COMB_DESC;
    while (i < len) {
        while (i < len && css_space((unsigned char)sel[i])) i++;
        if (i >= len) break;

        if (sel[i] == '>') { comb = CSS_COMB_CHILD; i++; continue; }
        if (sel[i] == '+' || sel[i] == '~') return 0;   /* siblings: not modelled */

        if (r->nparts >= CSS_MAX_PARTS) return 0;
        int n = css_parse_simple(sel + i, len - i, &r->part[r->nparts], &r->spec);
        if (!n) return 0;
        i += n;
        if (r->nparts > 0) r->comb[r->nparts - 1] = (unsigned char)comb;
        r->nparts++;
        comb = CSS_COMB_DESC;
    }
    if (!r->nparts) return 0;

    r->style = *st;
    r->important = important;

    /* File it under the most selective thing its subject requires. An element
     * without that tag, id or class cannot match, so it never looks here. */
    const css_simple *subj = &r->part[r->nparts - 1];
    int idx = css_nrules;
    int b;
    if (subj->id)        b = (int)css_hash(css_name(subj->id));
    else if (subj->ncls) b = (int)css_hash(css_name(subj->cls[0]));
    else if (subj->tag)  b = (int)css_hash(css_name(subj->tag));
    else                 b = -1;

    if (b < 0) { css_bnext[idx] = css_buniv; css_buniv = idx; }
    else       { css_bnext[idx] = css_bhead[b]; css_bhead[b] = idx; }

    css_nrules++;
    return 1;
}

/* --- media queries --------------------------------------------------------
 *
 * Most of a modern stylesheet lives inside one of these, so skipping them
 * whole -- which is what this used to do -- threw away the majority of every
 * page's rules. Only what can be answered is answered: the medium, and widths
 * against the window we actually have, written either way round. An
 * unrecognised condition is taken as true, because the author expected
 * SOMETHING to apply and the usual unknown is a feature that would have been
 * satisfied. A width we CAN read is answered honestly, which is the whole
 * point: otherwise every breakpoint in the sheet holds at once.
 */
enum { CSS_OP_NONE = 0, CSS_OP_LT, CSS_OP_LE, CSS_OP_GT, CSS_OP_GE, CSS_OP_EQ };

/* Read <, <=, >, >= or = and step past it. */
static int css_media_op(const char *q, int len, int *ip) {
    int i = *ip, op = CSS_OP_NONE;
    while (i < len && css_space((unsigned char)q[i])) i++;
    if (i < len && q[i] == '<') {
        op = CSS_OP_LT; i++;
        if (i < len && q[i] == '=') { op = CSS_OP_LE; i++; }
    } else if (i < len && q[i] == '>') {
        op = CSS_OP_GT; i++;
        if (i < len && q[i] == '=') { op = CSS_OP_GE; i++; }
    } else if (i < len && q[i] == '=') {
        op = CSS_OP_EQ; i++;
    }
    *ip = i;
    return op;
}

static int css_media_cmp(int a, int op, int b) {
    switch (op) {
    case CSS_OP_LT: return a <  b;
    case CSS_OP_LE: return a <= b;
    case CSS_OP_GT: return a >  b;
    case CSS_OP_GE: return a >= b;
    case CSS_OP_EQ: return a == b;
    }
    return 1;
}

/* One parenthesised condition, without its brackets. Three shapes are in use:
 *
 *     (min-width: 700px)            the old one
 *     (width >= 700px)              the range one
 *     (700px <= width <= 900px)     and its two-ended form
 *
 * Only the first was ever read, and an unread condition was taken as true.
 * On a stylesheet written in the range syntax that means every breakpoint
 * holds at once -- the phone layout and the desktop layout together, each
 * undoing the other. github.com has eight hundred and ninety-five of these
 * and not one written the old way.
 */
static int css_media_feature(const char *q, int len) {
    int i = 0;
    while (i < len && css_space((unsigned char)q[i])) i++;

    /* The value written first: (700px <= width), and possibly a second
     * bound after the feature name. */
    if (i < len && q[i] >= '0' && q[i] <= '9') {
        int vs = i;
        while (i < len && q[i] != '<' && q[i] != '>' && q[i] != '=') i++;
        int lo = css_length_px(q + vs, i - vs);
        int op = css_media_op(q, len, &i);

        while (i < len && css_space((unsigned char)q[i])) i++;
        char feat[CSS_NAME_MAX];
        i += css_ident(q + i, len - i, feat, CSS_NAME_MAX);
        if (!css_ieq(feat, "width") && !css_ieq(feat, "device-width")) return 1;
        if (lo >= 0 && !css_media_cmp(lo, op, css_viewport_w)) return 0;

        while (i < len && css_space((unsigned char)q[i])) i++;
        if (i < len && (q[i] == '<' || q[i] == '>' || q[i] == '=')) {
            int op2 = css_media_op(q, len, &i);
            int hi = css_length_px(q + i, len - i);
            if (hi >= 0 && !css_media_cmp(css_viewport_w, op2, hi)) return 0;
        }
        return 1;
    }

    char feat[CSS_NAME_MAX];
    i += css_ident(q + i, len - i, feat, CSS_NAME_MAX);
    while (i < len && css_space((unsigned char)q[i])) i++;

    if (i < len && q[i] == ':') {
        i++;
        int val = css_length_px(q + i, len - i);
        if (css_ieq(feat, "min-width") || css_ieq(feat, "min-device-width"))
            return !(val >= 0 && css_viewport_w < val);
        if (css_ieq(feat, "max-width") || css_ieq(feat, "max-device-width"))
            return !(val >= 0 && css_viewport_w > val);
        if (css_ieq(feat, "prefers-color-scheme")) {
            /* A site shipping both themes should give us the one that suits
             * the ground we are drawing on. */
            int wants_dark = 0;
            for (int k = i; k < len; k++)
                if (css_lower((unsigned char)q[k]) == 'd') { wants_dark = 1; break; }
            return wants_dark == (css_dark != 0);
        }
        return 1;     /* colour depth, orientation, a preference: assumed */
    }

    if (i < len && (q[i] == '<' || q[i] == '>' || q[i] == '=')) {
        int op = css_media_op(q, len, &i);
        int val = css_length_px(q + i, len - i);
        if (!css_ieq(feat, "width") && !css_ieq(feat, "device-width")) return 1;
        return val < 0 ? 1 : css_media_cmp(css_viewport_w, op, val);
    }

    return 1;         /* a bare feature -- (hover), (color) -- we cannot answer */
}

static int css_media_one(const char *q, int len) {
    int i = 0, ok = 1, negate = 0;
    while (i < len) {
        while (i < len && css_space((unsigned char)q[i])) i++;
        if (i >= len) break;

        if (q[i] == '(') {
            i++;
            int close = i, depth = 0;
            while (close < len && (depth || q[close] != ')')) {
                if (q[close] == '(') depth++;
                else if (q[close] == ')') depth--;
                close++;
            }
            if (!css_media_feature(q + i, close - i)) ok = 0;
            i = close < len ? close + 1 : close;
            continue;
        }

        char word[CSS_NAME_MAX];
        int n = css_ident(q + i, len - i, word, CSS_NAME_MAX);
        if (!n) { i++; continue; }
        i += n;

        if (css_ieq(word, "not")) negate = !negate;
        else if (css_ieq(word, "and") || css_ieq(word, "only")) { /* nothing */ }
        else if (css_ieq(word, "print") || css_ieq(word, "speech")) ok = 0;
        /* screen, all, and any medium we do not know: left alone */
    }
    return negate ? !ok : ok;
}

/* A query list: any one of them applying is enough. */
static int css_media_ok(const char *q, int len) {
    int a = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || q[i] == ',') {
            if (i > a && css_media_one(q + a, i - a)) return 1;
            a = i + 1;
        }
    }
    return 0;
}

/* Does any selector in a comma-separated list reach this element? Used to
 * decide which of a site's several themes to take its variables from -- each
 * set is guarded by a selector on the root, so asking the ordinary matcher
 * settles it. */
static int css_match_from(const css_rule_t *r, int pi, const css_elem *chain, int ei);

static int css_one_hits(const char *sel, int len, const css_elem *root, int n) {
    css_rule_t r;
    memset(&r, 0, sizeof r);
    int i = 0, comb = CSS_COMB_DESC;
    while (i < len) {
        while (i < len && css_space((unsigned char)sel[i])) i++;
        if (i >= len) break;
        if (sel[i] == '>') { comb = CSS_COMB_CHILD; i++; continue; }
        if (sel[i] == '+' || sel[i] == '~') return 0;
        if (r.nparts >= CSS_MAX_PARTS) return 0;
        int spec = 0;
        int k = css_parse_simple(sel + i, len - i, &r.part[r.nparts], &spec);
        if (!k) return 0;
        i += k;
        if (r.nparts > 0) r.comb[r.nparts - 1] = (unsigned char)comb;
        r.nparts++;
        comb = CSS_COMB_DESC;
    }
    if (!r.nparts) return 0;
    return css_match_from(&r, r.nparts - 1, root, n - 1);   /* an index, not a count */
}

static int css_selector_hits(const char *sel, int len, const css_elem *root, int n) {
    if (n <= 0) return 1;               /* no root given: take them all */
    int a = 0, par = 0, brk = 0;
    for (int k = 0; k <= len; k++) {
        if (k < len) {
            if (sel[k] == '(') par++;
            else if (sel[k] == ')' && par) par--;
            else if (sel[k] == '[') brk++;
            else if (sel[k] == ']' && brk) brk--;
        }
        if (k == len || (sel[k] == ',' && !par && !brk)) {
            if (css_one_hits(sel + a, k - a, root, n)) return 1;
            a = k + 1;
        }
    }
    return 0;
}

/* Keep a stylesheet until every one of them has arrived. Nothing can be
 * parsed yet: a value may name a custom property that a later sheet defines. */
static void css_add_source(const char *s, int len) {
    if (!css_src || len <= 0) return;
    if (css_srclen + len + 2 > CSS_SRC_CAP) len = CSS_SRC_CAP - css_srclen - 2;
    if (len <= 0) return;
    memcpy(css_src + css_srclen, s, (size_t)len);
    css_srclen += len;
    css_src[css_srclen++] = '\n';
    css_src[css_srclen] = 0;
}

enum { CSS_PASS_VARS = 0, CSS_PASS_RULES };

/* One pass over the buffered stylesheets. On the first it is looking only for
 * custom properties, and only in rules that apply to the root element -- that
 * is how a site's theme is chosen, since each theme's variables are guarded by
 * a selector on <html>. On the second it adds the rules for real. */
static void css_scan(const char *s, int len, int pass,
                     const css_elem *root, int nroot) {
    if (!css_rules) return;
    int i = 0;
    while (i < len) {
        if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (css_space((unsigned char)s[i])) { i++; continue; }

        if (s[i] == '@') {
            int ns = ++i;
            while (i < len && !css_space((unsigned char)s[i]) &&
                   s[i] != '{' && s[i] != ';') i++;
            char at[CSS_NAME_MAX];
            int nl = i - ns;
            if (nl >= CSS_NAME_MAX) nl = CSS_NAME_MAX - 1;
            for (int k = 0; k < nl; k++) at[k] = (char)css_lower((unsigned char)s[ns + k]);
            at[nl] = 0;

            int qs = i;
            while (i < len && s[i] != '{' && s[i] != ';') i++;
            int qe = i;

            if (i < len && s[i] == '{') {
                int depth = 1, bs = ++i;
                while (i < len && depth) {
                    if (s[i] == '{') depth++;
                    else if (s[i] == '}') depth--;
                    if (depth) i++;
                }
                int be = i;
                if (i < len) i++;

                /* @media and @supports wrap ordinary rules; the rest wrap
                 * things there is nothing here to do with. */
                if (css_ieq(at, "media")) {
                    if (css_media_ok(s + qs, qe - qs))
                        css_scan(s + bs, be - bs, pass, root, nroot);
                } else if (css_ieq(at, "supports") || css_ieq(at, "layer") ||
                           css_ieq(at, "scope")) {
                    css_scan(s + bs, be - bs, pass, root, nroot);
                }
            } else if (i < len) {
                i++;                            /* @import, @charset: skipped */
            }
            continue;
        }

        int ss = i;
        while (i < len && s[i] != '{' && s[i] != '}') i++;
        if (i >= len || s[i] != '{') { i++; continue; }
        int se = i++;
        int ds = i;
        int depth = 1;
        while (i < len && depth) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') depth--;
            if (depth) i++;
        }
        int de = i;
        if (i < len) i++;

        if (pass == CSS_PASS_VARS) {
            /* Does this rule define any custom properties at all? Nearly none
             * do, so the cheap test comes first. */
            int has_var = 0;
            for (int k = ds; k + 1 < de; k++)
                if (s[k] == '-' && s[k + 1] == '-' &&
                    (k == ds || s[k - 1] == ';' || s[k - 1] == '{' ||
                     css_space((unsigned char)s[k - 1]))) { has_var = 1; break; }
            if (!has_var) continue;

            /* Only the definitions that reach the root element count. That is
             * what picks one theme's palette out of the four a site ships. */
            if (!css_selector_hits(s + ss, se - ss, root, nroot)) continue;

            int k = ds;
            while (k < de) {
                while (k < de && (css_space((unsigned char)s[k]) || s[k] == ';')) k++;
                int ns2 = k;
                while (k < de && s[k] != ':' && s[k] != ';') k++;
                if (k >= de || s[k] != ':') break;
                int ne2 = k++;
                int vs2 = k, par2 = 0;
                while (k < de && (par2 || s[k] != ';')) {
                    if (s[k] == '(') par2++;
                    else if (s[k] == ')' && par2) par2--;
                    k++;
                }
                while (ns2 < ne2 && css_space((unsigned char)s[ns2])) ns2++;
                if (ne2 - ns2 > 2 && s[ns2] == '-' && s[ns2 + 1] == '-')
                    css_var_set(s + ns2, ne2 - ns2, s + vs2, k - vs2);
            }
            continue;
        }

        css_style_t st;
        css_style_init(&st);
        unsigned int touched = 0, important = 0;
        css_decls_full(&st, &touched, &important, s + ds, de - ds);
        if (!touched) continue;                 /* nothing here we can act on */

        /* Split the selector list on commas outside brackets. */
        int a = ss, par = 0, brk = 0;
        for (int k = ss; k <= se; k++) {
            if (k < se) {
                if (s[k] == '(') par++;
                else if (s[k] == ')' && par) par--;
                else if (s[k] == '[') brk++;
                else if (s[k] == ']' && brk) brk--;
            }
            if (k == se || (s[k] == ',' && !par && !brk)) {
                css_add_selector(s + a, k - a, &st, important);
                a = k + 1;
            }
        }
    }
}

/* --- matching ------------------------------------------------------------- */

static int css_attr_match(const css_simple *p, const css_elem *e) {
    if (!p->aop) return 1;
    if (!e->attr) return 0;

    const char *want = css_name(p->attr);
    const char *pval = css_name(p->aval);
    const char *a = e->attr;
    int alen = e->attrlen, wl = (int)strlen(want);
    for (int i = 0; i + wl <= alen; i++) {
        if (i && !css_space((unsigned char)a[i - 1])) continue;
        int k = 0;
        while (k < wl && css_lower((unsigned char)a[i + k]) == want[k]) k++;
        if (k != wl) continue;

        int j = i + wl;
        while (j < alen && css_space((unsigned char)a[j])) j++;
        if (j >= alen || a[j] != '=') return p->aop == CSS_AT_HAS;
        if (p->aop == CSS_AT_HAS) return 1;
        j++;
        while (j < alen && css_space((unsigned char)a[j])) j++;
        char q = 0;
        if (j < alen && (a[j] == '"' || a[j] == '\'')) q = a[j++];
        int vs = j;
        while (j < alen) {
            if (q ? a[j] == q : (css_space((unsigned char)a[j]) || a[j] == '>')) break;
            j++;
        }
        int vl = j - vs, pl = (int)strlen(pval);
        const char *v = a + vs;

        switch (p->aop) {
        case CSS_AT_EQ:     return vl == pl && !memcmp(v, pval, (size_t)pl);
        case CSS_AT_PREFIX: return pl <= vl && !memcmp(v, pval, (size_t)pl);
        case CSS_AT_SUFFIX: return pl <= vl && !memcmp(v + vl - pl, pval, (size_t)pl);
        case CSS_AT_SUB:
            for (int t = 0; t + pl <= vl; t++)
                if (!memcmp(v + t, pval, (size_t)pl)) return 1;
            return 0;
        case CSS_AT_WORD: {
            char buf[CSS_NAME_MAX * 4];
            int n2 = vl < (int)sizeof buf - 1 ? vl : (int)sizeof buf - 1;
            memcpy(buf, v, (size_t)n2);
            buf[n2] = 0;
            return css_in_list(buf, pval);
        }
        }
        return 0;
    }
    return 0;
}

static int css_simple_match(const css_simple *p, const css_elem *e, int is_root) {
    if (p->pseudo == CSS_PS_NEVER) return 0;
    if (p->tag && !(e->tag && css_ieq(e->tag, css_name(p->tag)))) return 0;
    if (p->id && !(e->id && css_ieq(e->id, css_name(p->id)))) return 0;
    for (int i = 0; i < p->ncls; i++)
        if (!css_in_list(e->cls, css_name(p->cls[i]))) return 0;
    if (p->pseudo == CSS_PS_FIRST && !e->first) return 0;
    if (p->pseudo == CSS_PS_LAST && !e->last) return 0;
    if (p->pseudo == CSS_PS_ROOT && !is_root) return 0;
    if (!css_attr_match(p, e)) return 0;
    return 1;
}

/* Right to left, backtracking on the descendant combinator. Four parts at
 * most, so doing it properly costs nothing. */
static int css_match_from(const css_rule_t *r, int pi, const css_elem *chain, int ei) {
    if (ei < 0) return 0;
    if (!css_simple_match(&r->part[pi], &chain[ei], ei == 0)) return 0;
    if (pi == 0) return 1;
    if (r->comb[pi - 1] == CSS_COMB_CHILD)
        return css_match_from(r, pi - 1, chain, ei - 1);
    for (int k = ei - 1; k >= 0; k--)
        if (css_match_from(r, pi - 1, chain, k)) return 1;
    return 0;
}

/* Fold every rule that applies to chain[n-1] into `out`, more specific and
 * later rules winning. `out` should arrive EMPTY: the caller layers
 * inheritance and the style attribute around the result, because anything
 * already in here would have nothing to outrank it. */
/* Everything one element needs to remember while the candidates go past. */
typedef struct {
    long long disp, weight, scale, align, color, mt, mb, bg, decor, tcase;
    long long box, width, border, vis;
    long long fdir, cols, gap, pos, grow;
} css_won;

static void css_try_rule(int i, const css_elem *chain, int n,
                         css_style_t *out, css_won *w) {
    css_rule_t *r = &css_rules[i];
    if (!css_match_from(r, r->nparts - 1, chain, n - 1)) return;

    const css_style_t *s = &r->style;
    long long base = (long long)r->spec;
    /* !important lifts a declaration above every ordinary one whatever their
     * specificity; the rule's own index breaks the remaining ties, which is
     * "the later one wins". */
    #define CSS_W(bit) \
        ((base + ((r->important & (bit)) ? 1000000 : 0)) * 100000LL + i)

    if (s->display && CSS_W(CSSP_DISPLAY) > w->disp) {
        out->display = s->display; w->disp = CSS_W(CSSP_DISPLAY);
    }
    if (s->weight && CSS_W(CSSP_WEIGHT) > w->weight) {
        out->weight = s->weight; w->weight = CSS_W(CSSP_WEIGHT);
    }
    if (s->scale && CSS_W(CSSP_SCALE) > w->scale) {
        out->scale = s->scale; w->scale = CSS_W(CSSP_SCALE);
    }
    if (s->align && CSS_W(CSSP_ALIGN) > w->align) {
        out->align = s->align; w->align = CSS_W(CSSP_ALIGN);
    }
    if (s->has_color && CSS_W(CSSP_COLOR) > w->color) {
        out->color = s->color; out->has_color = 1; w->color = CSS_W(CSSP_COLOR);
    }
    if (s->has_bg && CSS_W(CSSP_BG) > w->bg) {
        out->bg = s->bg; out->has_bg = 1; w->bg = CSS_W(CSSP_BG);
    }
    if (s->decor && CSS_W(CSSP_DECOR) > w->decor) {
        out->decor = s->decor; w->decor = CSS_W(CSSP_DECOR);
    }
    if (s->tcase && CSS_W(CSSP_TCASE) > w->tcase) {
        out->tcase = s->tcase; w->tcase = CSS_W(CSSP_TCASE);
    }
    if (s->vis && CSS_W(CSSP_VIS) > w->vis) {
        out->vis = s->vis; w->vis = CSS_W(CSSP_VIS);
    }
    /* One bit for !important, but a weight each: a page sets the direction
     * and the column count from rules that have nothing to do with each
     * other, and the more specific of them must not silence the other. */
    if (s->fdir && CSS_W(CSSP_FLOW) > w->fdir) {
        out->fdir = s->fdir; w->fdir = CSS_W(CSSP_FLOW);
    }
    if (s->cols && CSS_W(CSSP_FLOW) > w->cols) {
        out->cols = s->cols; w->cols = CSS_W(CSSP_FLOW);
    }
    if (s->gap >= 0 && CSS_W(CSSP_GAP) > w->gap) {
        out->gap = s->gap; w->gap = CSS_W(CSSP_GAP);
    }
    if (s->pos && CSS_W(CSSP_POS) > w->pos) {
        out->pos = s->pos; w->pos = CSS_W(CSSP_POS);
    }
    if (s->grow && CSS_W(CSSP_GROW) > w->grow) {
        out->grow = s->grow; w->grow = CSS_W(CSSP_GROW);
    }
    if (s->mt >= 0 && CSS_W(CSSP_MT) > w->mt) { out->mt = s->mt; w->mt = CSS_W(CSSP_MT); }
    if (s->mb >= 0 && CSS_W(CSSP_MB) > w->mb) { out->mb = s->mb; w->mb = CSS_W(CSSP_MB); }

    if (CSS_W(CSSP_BOX) > w->box) {
        int any = 0;
        if (s->ml != -1) { out->ml = s->ml; any = 1; }
        if (s->mr != -1) { out->mr = s->mr; any = 1; }
        if (s->pt >= 0)  { out->pt = s->pt; any = 1; }
        if (s->pb >= 0)  { out->pb = s->pb; any = 1; }
        if (s->pl >= 0)  { out->pl = s->pl; any = 1; }
        if (s->pr >= 0)  { out->pr = s->pr; any = 1; }
        if (any) w->box = CSS_W(CSSP_BOX);
    }
    if ((s->w_px != -1 || s->w_pct >= 0 || s->maxw_px != -1 || s->maxw_pct >= 0) &&
        CSS_W(CSSP_WIDTH) > w->width) {
        if (s->w_px != -1 || s->w_pct >= 0) { out->w_px = s->w_px; out->w_pct = s->w_pct; }
        if (s->maxw_px != -1 || s->maxw_pct >= 0) {
            out->maxw_px = s->maxw_px; out->maxw_pct = s->maxw_pct;
        }
        w->width = CSS_W(CSSP_WIDTH);
    }
    if ((s->bt >= 0 || s->bb >= 0 || s->bl >= 0 || s->br >= 0 || s->has_bcolor) &&
        CSS_W(CSSP_BORDER) > w->border) {
        if (s->bt >= 0) out->bt = s->bt;
        if (s->bb >= 0) out->bb = s->bb;
        if (s->bl >= 0) out->bl = s->bl;
        if (s->br >= 0) out->br = s->br;
        if (s->has_bcolor) { out->bcolor = s->bcolor; out->has_bcolor = 1; }
        w->border = CSS_W(CSSP_BORDER);
    }
    #undef CSS_W
}

static void css_match_chain(const css_elem *chain, int n, css_style_t *out) {
    if (!css_rules || n <= 0) return;
    const css_elem *e = &chain[n - 1];

    css_won w;
    w.disp = w.weight = w.scale = w.align = w.color = -1;
    w.mt = w.mb = w.bg = w.decor = w.tcase = -1;
    w.box = w.width = w.border = w.vis = -1;
    w.fdir = w.cols = w.gap = w.pos = w.grow = -1;

    /* The rules that could possibly apply: the ones filed under this
     * element's tag, its id, each of its classes, and the ones filed under
     * nothing at all. */
    for (int i = css_buniv; i >= 0; i = css_bnext[i])
        css_try_rule(i, chain, n, out, &w);

    if (e->tag)
        for (int i = css_bhead[css_hash(e->tag)]; i >= 0; i = css_bnext[i])
            css_try_rule(i, chain, n, out, &w);

    if (e->id)
        for (int i = css_bhead[css_hash(e->id)]; i >= 0; i = css_bnext[i])
            css_try_rule(i, chain, n, out, &w);

    if (e->cls) {
        const char *p = e->cls;
        while (*p) {
            while (*p && css_space((unsigned char)*p)) p++;
            const char *s2 = p;
            while (*p && !css_space((unsigned char)*p)) p++;
            if (p > s2) {
                char one[CSS_NAME_MAX];
                int l = (int)(p - s2);
                if (l >= CSS_NAME_MAX) l = CSS_NAME_MAX - 1;
                for (int k = 0; k < l; k++) one[k] = (char)css_lower((unsigned char)s2[k]);
                one[l] = 0;
                for (int i = css_bhead[css_hash(one)]; i >= 0; i = css_bnext[i])
                    css_try_rule(i, chain, n, out, &w);
            }
        }
    }
}

/* Read everything that has arrived: the custom properties first, because a
 * value cannot be understood until the names in it stand for something, then
 * the rules. `root` is the page's root element -- what <html> looks like --
 * and it is what decides which theme's palette is taken. */
static void css_build(const css_elem *root, int nroot) {
    if (!css_src || !css_srclen) return;
    css_scan(css_src, css_srclen, CSS_PASS_VARS, root, nroot);
    css_scan(css_src, css_srclen, CSS_PASS_RULES, root, nroot);
}

/* --- readability ---------------------------------------------------------- */

/* Perceived brightness, 0..255. Green carries most of it, blue almost none. */
static int css_luma(unsigned int c) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    return (r * 54 + g * 183 + b * 19) >> 8;
}

/* A site picks its colours for its own background, not ours. A page designed
 * in black on white, shown on a dark ground, would be black on black -- so a
 * colour is only honoured when it still separates from what is behind it. */
static int css_readable(unsigned int fg, unsigned int bg) {
    int d = css_luma(fg) - css_luma(bg);
    if (d < 0) d = -d;
    return d >= 45;
}

#endif /* CSS_H */
