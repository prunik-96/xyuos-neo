/* web -- a browser.
 *
 * It fetches a page, turns the HTML into a stream of things to draw, wraps
 * them to the window, and lets you click the links. That is the whole of it.
 *
 * It reads a useful subset of CSS (see css.h): colours, weights, sizes and
 * whether a thing is shown at all, from style attributes, <style> blocks and
 * linked stylesheets, cascaded by specificity. It does not do JavaScript, and
 * it does not do positioning -- a page is laid out the way its markup says it
 * is STRUCTURED, not where the author wanted the boxes. That reads better than
 * it sounds: most pages are still a document underneath, and the document is
 * what comes through.
 *
 * The font is monospace, which is the one the window manager has, so layout is
 * measured in character cells. Headings are the same glyphs drawn at double
 * size. Bold is the same glyph drawn twice, a pixel apart.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"
#include "gui.h"
#include "img.h"
#include "css.h"
#include "dom.h"
#include "js.h"
#include "jsdom.h"

#define TOOL_H  38
#define STAT_H  24
#define MARGIN  12

/* --- limits ---------------------------------------------------------------
 * Generous, but fixed: a browser that grows its buffers to fit whatever it is
 * given is a browser that can be handed something enormous. */
#define MAX_HTML  (768 * 1024)
#define MAX_TEXT  (384 * 1024)
#define MAX_ITEMS  60000
#define MAX_LINKS   6000
#define MAX_HREF  (192 * 1024)
#define WORD_SPLIT 40        /* longest run of characters kept unbroken */
#define MAX_IMAGES 64        /* pictures fetched for one page */
#define MAX_IMG_BYTES (3 * 1024 * 1024)
/* A site splits its styles across many files -- a theme, a base, a bundle per
 * component. Six was enough to hold the themes and nothing else, so the rules
 * never arrived. The real bound is the source buffer in css.h. */
#define MAX_SHEETS 24        /* external stylesheets fetched for one page */

/* --- what a page becomes -------------------------------------------------- */

enum { IT_WORD, IT_BREAK, IT_PARA, IT_RULE, IT_BULLET, IT_IMAGE, IT_FIELD };
enum { ST_TEXT, ST_LINK, ST_BOLD, ST_HEAD, ST_CODE };

typedef struct {
    int   off, len;          /* into textbuf */
    int   x, y;              /* filled in by layout, in pixels */
    /* A picture's width as laid out. It was worked out from the box the
     * picture sits in -- a column of a row, a cell of a table -- and then
     * worked out again at drawing time from the whole window, which is a
     * different and much larger number. The picture was then painted across
     * whatever stood beside it. Measured once, kept, and used everywhere. */
    int   wpx;
    short link;              /* index into links[], or -1 */
    int   node;              /* the element it came from, for clicks */
    unsigned char kind, style, scale;
    unsigned char glue;      /* no space before this word: none was written */
    unsigned char has_color; /* the page asked for a colour of its own */
    unsigned int  color;
    unsigned char decor;     /* text-decoration, if the page named one */
    unsigned char on_bg;     /* the page painted something behind this */
    unsigned int  ground;    /* and this is what */
} item_t;

/* --- form fields -----------------------------------------------------------
 *
 * A page's forms were, until now, holes: <input> is a void tag with no text,
 * so a search box rendered as nothing at all and the label above it explained
 * an empty space. These are the things you can type into, tick, and press.
 *
 * Each one keeps its own value, because that is the only place the value
 * lives -- the markup says what it started as, and after that the field is
 * the record. */
enum { FLD_TEXT, FLD_PASSWORD, FLD_CHECK, FLD_RADIO, FLD_SELECT,
       FLD_AREA, FLD_SUBMIT, FLD_BUTTON, FLD_HIDDEN };

#define MAX_FIELDS  256
#define FIELD_VAL   512
#define FIELD_NAME  96
#define MAX_OPTS    32

typedef struct {
    int  node;                   /* the element it came from */
    int  form;                   /* the <form> above it, or -1 */
    unsigned char kind;
    unsigned char checked;
    unsigned char disabled;
    short cols, rows;            /* the size of the box, in characters */
    int   cur;                   /* where the caret sits, in characters */
    int   nopt, opt;             /* a select: how many choices, and which */
    char  name[FIELD_NAME];
    char  value[FIELD_VAL];
    char  label[FIELD_NAME];     /* what a button says */
    char  optval[MAX_OPTS][48];  /* what each choice submits */
    char  optlab[MAX_OPTS][48];  /* and what it reads as */
} field_t;

static field_t fields[MAX_FIELDS];
static int nfields;
static int focus_field = -1;

/* Defined down beside the fetching. Everything a page is built out of goes
 * through this rather than fetch_url. */
static int fetch_cached(const char *url, char *buf, int max);

/* Also defined down there, and used up here where a response is taken apart
 * and a request is put together. */
static void cookie_take(const char *line, const char *host, const char *path,
                        int over_tls);
static void cookie_header(const char *host, const char *path, int over_tls,
                          char *out, int max);

/* --- tabs -------------------------------------------------------------------
 *
 * Everything about the page on screen is a global: the tree, the drawing
 * list, the stylesheet, the fields, the script heap. Those are megabytes
 * apiece and there is no sense in six of each, so a tab does not keep them.
 *
 * A tab keeps what is small and cannot be worked out again -- where it is,
 * what it is called, how far down it was read, and the road behind and ahead
 * of it -- plus the page's own bytes. Switching to a tab reads those bytes
 * again, which costs a parse and not a journey to the other side of the
 * world; the stylesheets it needs come out of the cache.
 *
 * What that loses is honest to say: what was typed into a form, what a script
 * had done to the page, and anything the page was in the middle of.
 */
#define MAX_TABS 6
#define TAB_H    22

/* What was filled in, kept by the element it was filled into. Rebuilding the
 * page from the same bytes gives the same tree and so the same numbering,
 * which is what makes matching by node work here at all. */
#define TAB_FIELDS 64

typedef struct {
    int   node;
    unsigned char kind, checked;
    int   opt;
    char  value[256];
} tabfield_t;

typedef struct {
    char  url[1024];
    char  title[160];
    char *src;                     /* the page's own bytes, or none yet */
    int   srclen;
    int   scroll;
    int   nhist, nfwd;
    char  (*hist)[1024];
    char  (*fwd)[1024];
    tabfield_t fld[TAB_FIELDS];
    int   nfld;
} tab_t;

static tab_t tabs[MAX_TABS];
static int   ntabs = 1;
static int   cur_tab;

/* The strip only exists when there is more than one tab, so with a single
 * tab the page sits exactly where it always did. */
#define VIEW_TOP (TOOL_H + (ntabs > 1 ? TAB_H : 0))

static char   *html;
static unsigned short *textbuf;   static int textlen;   /* code points */
static item_t *items;     static int nitems;

/* --- find on the page ------------------------------------------------------
 *
 * The drawing list is one flat run of words, so a phrase that crosses a word
 * boundary crosses an item boundary too, and a match is a walk over items
 * rather than a search inside a string. A space in the query matches the gap
 * between two words -- and a line break, a paragraph, a bullet: anything that
 * is not a word is the same gap, because on the page it reads as one.
 *
 * A match is kept as the pieces it covers, one per word it runs through, so
 * the highlight follows it across a line break instead of stopping at the
 * first word.
 */
#define FIND_MAX_Q      64
#define FIND_MAX_SPANS  2048

typedef struct {
    int item, off, len;      /* a run of characters inside one word */
    int match;               /* which match it belongs to */
} find_span;

static char      find_query[FIND_MAX_Q];
static int       find_open;          /* the bar is showing */
static find_span find_spans[FIND_MAX_SPANS];
static int       find_nspans, find_nmatch, find_cur;

static int find_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Does the query start here, inside word `i` at character `off`? When `rec`
 * is set the pieces it covers are written down as it goes. */
static int find_here(int i, int off, int rec) {
    int qi = 0;
    int first = find_nspans;
    for (;;) {
        char q = find_query[qi];
        if (!q) return 1;
        if (i >= nitems) break;
        item_t *it = &items[i];

        if (it->kind != IT_WORD) {
            if (q != ' ') break;
            while (i < nitems && items[i].kind != IT_WORD) i++;
            qi++;
            off = 0;
            continue;
        }
        if (off >= it->len) {
            if (q != ' ') break;
            qi++; i++; off = 0;
            continue;
        }

        int begin = off;
        while (find_query[qi] && find_query[qi] != ' ' && off < it->len) {
            int c = textbuf[it->off + off];
            if (c > 127) break;
            if (find_lower(c) != find_query[qi]) break;
            qi++; off++;
        }
        if (off == begin) break;                  /* nothing matched here */
        if (rec && find_nspans < FIND_MAX_SPANS) {
            find_span *sp = &find_spans[find_nspans++];
            sp->item = i; sp->off = begin; sp->len = off - begin;
            sp->match = find_nmatch;
        }
        if (!find_query[qi]) return 1;
        if (find_query[qi] != ' ' && off < it->len) break;   /* a real mismatch */
    }
    if (rec) find_nspans = first;                 /* unwind what did not match */
    return 0;
}

/* Every place the query appears, in the order they are read. */
static void find_scan(void) {
    find_nspans = find_nmatch = 0;
    if (!find_query[0]) { find_cur = 0; return; }
    for (int i = 0; i < nitems && find_nspans < FIND_MAX_SPANS; i++) {
        if (items[i].kind != IT_WORD) continue;
        for (int off = 0; off < items[i].len; off++) {
            if (!find_here(i, off, 0)) continue;
            int before = find_nspans;
            if (!find_here(i, off, 1)) continue;
            find_nmatch++;
            /* Step past what this match covered, so "aaa" in "aaaa" is one
             * hit and not two overlapping ones. */
            if (find_nspans > before && find_spans[before].item == i)
                off += find_spans[before].len - 1;
        }
    }
    if (find_cur >= find_nmatch) find_cur = 0;
    if (find_cur < 0) find_cur = find_nmatch ? find_nmatch - 1 : 0;
}

/* The first piece of a match, for scrolling to it. */
static int find_span_of(int match) {
    for (int i = 0; i < find_nspans; i++)
        if (find_spans[i].match == match) return i;
    return -1;
}

/* Do the characters at `off` of word `i` fall inside a match? Returns the
 * match number, or -1. Spans come out of the scan in item order, so this is
 * a bisection rather than a walk. */
static int find_hit(int item, int off) {
    int lo = 0, hi = find_nspans - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (find_spans[mid].item < item) lo = mid + 1;
        else if (find_spans[mid].item > item) hi = mid - 1;
        else {
            /* The right word; now find the piece covering this character. */
            int k = mid;
            while (k > 0 && find_spans[k - 1].item == item) k--;
            for (; k < find_nspans && find_spans[k].item == item; k++)
                if (off >= find_spans[k].off &&
                    off < find_spans[k].off + find_spans[k].len)
                    return find_spans[k].match;
            return -1;
        }
    }
    return -1;
}
static char   *hrefbuf;   static int hreflen;
static int    *linkoff;   static int nlinks;

static char page_title[128];
static int  doc_height;

/* What the last page cost to turn into words. Shown in the network panel,
 * because the cascade is the expensive half of drawing a page and it should
 * be possible to read that rather than guess it. */
static int page_elems, page_tree_ms, page_style_ms, page_render_ms;
static int page_css_fetch_ms;   /* how much of the style time was the network */

/* Every element that asked for a background, and where its words ended up.
 * There are no boxes in this layout, so the extent of one is worked out
 * afterwards from the words that came out of it. */
#define MAX_BGBOX 400
typedef struct {
    int node;
    unsigned int color;
    int block;                  /* spans the column, rather than the words */
    int x0, y0, x1, y1;
} bgbox_t;
/* What layout needs from each element: the style it computed to, and the
 * stretch of the drawing list that came out of it. Items are produced in
 * document order, so a subtree's items are always one contiguous run -- which
 * is what makes it possible to lay out a tree of boxes over a flat list. */
static css_style_t *node_style;
static int *node_i0, *node_i1;          /* i0 < 0 means it was never drawn */

/* A box that will be painted: a background, a border, or both. Only elements
 * that paint get one; the rest have their geometry and forget it. */
#define MAX_BOX 800
typedef struct {
    int x, y, w, h;
    unsigned char has_bg;
    unsigned char bt, bb, bl, br;      /* which edges carry a line */
    unsigned int  bg, bcolor;
} box_t;
static box_t boxes[MAX_BOX];
static int nboxes;

/* Whether the drawing list has been through layout. Between building it and
 * measuring it every item sits at (0,0), and anything that painted in that
 * window would stack the whole page in one place -- which is exactly what
 * fetching a script mid-load started doing. Rather than rely on the order of
 * two calls staying right forever, the painter refuses to draw a list that
 * has not been measured. */
static int items_laid_out;

/* --- pictures --------------------------------------------------------------
 * Each one is fetched separately, after the page itself is already on screen.
 * A picture that has not arrived still takes up its space, so the text does
 * not jump around underneath the reader when it does. */

enum { PIC_WANT, PIC_OK, PIC_FAIL };

typedef struct {
    char    url[600];
    image_t im;              /* decoded and already scaled to fit */
    int     state;
    int     want_w, want_h;  /* from the width= and height= attributes */
    char    alt[96];         /* what the page says the picture shows */
} pic_t;

static pic_t pics[MAX_IMAGES];
static int   npics;
/* Why the last picture that did not come through did not come through. Shown
 * in the status line, because a blank gap on the page explains nothing. */
static char  pic_err[80];
static int   nsheets;
static unsigned char *dlbuf;      /* one picture's or stylesheet's bytes */

/* --- where we are --------------------------------------------------------- */

static char cur_url[1024];
static char status[256];

/* The window. Declared up here because the stylesheet has to be read against
 * a width -- a breakpoint is answered when the rules are filed, long before
 * anything is laid out. */
static gui_t *G;
static int  scroll;
static int  hover_link = -1;

#define HIST_MAX 32
static char hist[HIST_MAX][1024];      /* where we have been */
static int  nhist;
static char fwd[HIST_MAX][1024];       /* and where we came back from */
static int  nfwd;

/* Every request the system made, and what it cost. The interesting column is
 * whether a connection was reused: that request paid for no handshake, and the
 * milliseconds beside it say what that was worth. */
#define NET_ROWS   64
static int show_net;                   /* the panel is open */
static int net_scroll;

/* --- small helpers -------------------------------------------------------- */

static int ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* --- URLs ------------------------------------------------------------------
 * Split a URL into the three things a fetch needs, and resolve a relative one
 * against the page it was found on. */

static void url_split(const char *url, char *host, int hostmax,
                      char *path, int pathmax, int *port, int *tls) {
    const char *u = url;
    *tls = 0;
    *port = 80;
    if (strncmp(u, "https://", 8) == 0) { u += 8; *tls = 1; *port = 443; }
    else if (strncmp(u, "http://", 7) == 0) u += 7;

    int i = 0;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && i < hostmax - 1)
        host[i++] = *u++;
    host[i] = 0;
    while (*u && *u != '/' && *u != '?' && *u != '#') {
        if (*u == ':') {
            u++;
            *port = 0;
            while (*u >= '0' && *u <= '9') *port = *port * 10 + (*u++ - '0');
        } else u++;
    }
    i = 0;
    if (*u != '/') path[i++] = '/';
    /* A fragment is for the browser, not the server. */
    while (*u && *u != '#' && i < pathmax - 1) path[i++] = *u++;
    path[i] = 0;
    if (i == 0) { path[0] = '/'; path[1] = 0; }
}

static void url_resolve(const char *base, const char *rel, char *out, int max) {
    if (!rel || !rel[0]) { snprintf(out, max, "%s", base); return; }

    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        snprintf(out, max, "%s", rel);
        return;
    }

    char scheme[8], host[256], path[768];
    int port, tls;
    url_split(base, host, sizeof host, path, sizeof path, &port, &tls);
    snprintf(scheme, sizeof scheme, "%s", tls ? "https" : "http");

    if (rel[0] == '/' && rel[1] == '/') {          /* //host/path */
        snprintf(out, max, "%s:%s", scheme, rel);
        return;
    }
    if (rel[0] == '/') {                            /* /absolute/path */
        if ((tls && port != 443) || (!tls && port != 80))
            snprintf(out, max, "%s://%s:%d%s", scheme, host, port, rel);
        else
            snprintf(out, max, "%s://%s%s", scheme, host, rel);
        return;
    }
    if (rel[0] == '#') {                            /* same page */
        snprintf(out, max, "%s", base);
        return;
    }

    /* Relative to the directory the current page sits in. */
    char dir[768];
    snprintf(dir, sizeof dir, "%s", path);
    int cut = 0;
    for (int i = 0; dir[i]; i++) if (dir[i] == '/') cut = i;
    dir[cut + 1] = 0;

    char joined[1024];
    snprintf(joined, sizeof joined, "%s%s", dir, rel);

    /* Fold away . and .. segments. */
    char clean[1024];
    int c = 0;
    for (int i = 0; joined[i]; ) {
        if (joined[i] == '/' && joined[i+1] == '.' && joined[i+2] == '/') { i += 2; continue; }
        if (joined[i] == '/' && joined[i+1] == '.' && joined[i+2] == '.' &&
            (joined[i+3] == '/' || joined[i+3] == 0)) {
            while (c > 0 && clean[c-1] != '/') c--;
            if (c > 0) c--;
            i += 3;
            continue;
        }
        if (c < (int)sizeof clean - 1) clean[c++] = joined[i];
        i++;
    }
    clean[c] = 0;
    if (clean[0] != '/') { memmove(clean + 1, clean, (size_t)c + 1); clean[0] = '/'; }

    if ((tls && port != 443) || (!tls && port != 80))
        snprintf(out, max, "%s://%s:%d%s", scheme, host, port, clean);
    else
        snprintf(out, max, "%s://%s%s", scheme, host, clean);
}


/* --- text that is not ASCII ----------------------------------------------- */

/* One code point from a UTF-8 sequence. Returns the bytes consumed. Invalid
 * sequences are treated as single bytes so that no input can stall the loop. */
static int utf8_next(const char *s, int len, int *cp);

/* --- what the bytes of a page mean ------------------------------------------
 *
 * Every page was read as UTF-8, because that is what pages are written in
 * now. The ones that are not are exactly the ones a UTF-8 reader turns into
 * nonsense: a Russian page in windows-1251 comes out as pairs of accented
 * Latin letters, top to bottom, with nothing to suggest what went wrong.
 *
 * There is one place in this program where bytes become characters, so there
 * is one place to fix. The tables below are the upper half of each encoding;
 * the lower half is ASCII in all of them.
 */
enum { ENC_UTF8 = 0, ENC_CP1251, ENC_KOI8, ENC_CP1252 };

/* windows-1251: what almost every older Russian page is written in */
static const unsigned short enc_cp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

/* koi8-r: what the ones older than those are written in */
static const unsigned short enc_koi8[128] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248,
    0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556,
    0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565,
    0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,
    0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,
    0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,
};

/* windows-1252, which is also iso-8859-1 for everything above 0x9F */
static const unsigned short enc_cp1252[128] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

/* What the page being read is written in. */
static int page_enc = ENC_UTF8;

/* What the last response called itself. Filled in down beside the fetching;
 * declared here because the encoding is decided long before that code. */
static char last_ctype[128];
static char last_disp[256];

/* Name to encoding. Anything unrecognised stays UTF-8, which is the right
 * guess for a page whose author had an opinion we cannot read. */
static int enc_by_name(const char *n) {
    char t[32];
    int k = 0;
    for (const char *c = n; *c && k < (int)sizeof t - 1; c++) {
        if (*c == ' ' || *c == '"' || *c == '\'' || *c == '-' || *c == '_') continue;
        t[k++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    }
    t[k] = 0;
    if (!strncmp(t, "windows1251", 11) || !strncmp(t, "cp1251", 6) ||
        !strncmp(t, "x-cp1251", 8))                       return ENC_CP1251;
    if (!strncmp(t, "koi8", 4))                           return ENC_KOI8;
    if (!strncmp(t, "windows1252", 11) || !strncmp(t, "cp1252", 6) ||
        !strncmp(t, "iso88591", 8) || !strncmp(t, "latin1", 6)) return ENC_CP1252;
    return ENC_UTF8;
}

/* One character out of the page, whatever it is written in. */
static int text_next(const char *s, int len, int *cp) {
    if (page_enc == ENC_UTF8) return utf8_next(s, len, cp);
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    const unsigned short *t = page_enc == ENC_CP1251 ? enc_cp1251
                            : page_enc == ENC_KOI8   ? enc_koi8
                                                     : enc_cp1252;
    *cp = t[c - 128];
    (void)len;
    return 1;
}

static int utf8_next(const char *s, int len, int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }

    int need, v;
    if ((c & 0xE0) == 0xC0)      { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else { *cp = '?'; return 1; }

    if (len < need + 1) { *cp = '?'; return 1; }
    for (int i = 1; i <= need; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xC0) != 0x80) { *cp = '?'; return 1; }
        v = (v << 6) | (k & 0x3F);
    }
    *cp = v;
    return need + 1;
}



/* Keep what the font can draw; fold the rest onto something it can.
 *
 * The atlas covers ASCII, Latin-1, Latin Extended-A and Cyrillic, so most of
 * Europe arrives intact. Greek, Arabic, Chinese and the rest have no glyphs
 * and become '?', which at least says "a character was here". Returns how
 * many code points were written. */
static int cp_fold(int cp, unsigned short *out) {
    if (cp == 9 || cp == 10 || cp == 13) { out[0] = (unsigned short)cp; return 1; }
    if (gui_font_slot(cp) >= 0) { out[0] = (unsigned short)cp; return 1; }

    switch (cp) {
    case 0x00A0: out[0] = ' '; return 1;                 /* no-break space */
    case 0x00AB: out[0] = '<'; out[1] = '<'; return 2;
    case 0x00BB: out[0] = '>'; out[1] = '>'; return 2;
    case 0x00B7: case 0x2022: out[0] = '*'; return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: case 0x2212: out[0] = '-'; return 1;
    case 0x2018: case 0x2019: case 0x201A: out[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: out[0] = '"'; return 1;
    case 0x2026: out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;
    case 0x00D7: out[0] = 'x'; return 1;
    case 0x2192: out[0] = '-'; out[1] = '>'; return 2;
    case 0x2190: out[0] = '<'; out[1] = '-'; return 2;
    case 0x00AD: case 0x200B: case 0x200E: case 0x200F:
    case 0xFEFF: return 0;                               /* invisible */
    }

    out[0] = '?';
    return 1;
}

/* Fetching lives further down, but the parser needs it: a stylesheet has
 * to be pulled in the middle of reading the page that asked for it. */
static int fetch_url(const char *url, char *buf, int max,
                     char *final_url, int final_max);

/* --- pictures: fetching, decoding, fitting --------------------------------- */

/* Find the slot for a URL, or make one. The same picture used twice on a page
 * -- a bullet, a logo in a list -- is fetched once. */
static int pic_slot(const char *url) {
    for (int i = 0; i < npics; i++)
        if (strcmp(pics[i].url, url) == 0) return i;
    if (npics >= MAX_IMAGES) return -1;
    pic_t *p = &pics[npics];
    snprintf(p->url, sizeof p->url, "%s", url);
    p->state = PIC_WANT;
    p->im.px = 0; p->im.w = p->im.h = 0;
    p->want_w = p->want_h = 0;
    return npics++;
}

/* A picture that arrived inside the page itself, already drawn. There is no
 * address to keep it under, so it is given one nothing else will collide
 * with -- the slot is what matters, and the name is only a key. */
static int pic_inline(const image_t *im) {
    if (npics >= MAX_IMAGES) return -1;
    pic_t *p = &pics[npics];
    snprintf(p->url, sizeof p->url, "#inline-%d", npics);
    p->state = PIC_OK;
    p->im = *im;
    p->want_w = im->w;
    p->want_h = im->h;
    p->alt[0] = 0;
    return npics++;
}

static void pics_clear(void) {
    for (int i = 0; i < npics; i++) img_free(&pics[i].im);
    npics = 0;
}

/* Shrink to fit, averaging the pixels that collapse together. A photo scaled
 * by dropping rows looks like a photo that has been damaged. */
static int pic_fit(image_t *im, int maxw, int maxh) {
    if (im->w <= maxw && im->h <= maxh) return 1;

    int nw = im->w, nh = im->h;
    if (nw > maxw) { nh = (int)((long)nh * maxw / nw); nw = maxw; }
    if (nh > maxh) { nw = (int)((long)nw * maxh / nh); nh = maxh; }
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    unsigned int *out = (unsigned int *)malloc((size_t)nw * nh * 4);
    if (!out) return 0;

    for (int y = 0; y < nh; y++) {
        int sy0 = (int)((long)y * im->h / nh);
        int sy1 = (int)((long)(y + 1) * im->h / nh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < nw; x++) {
            int sx0 = (int)((long)x * im->w / nw);
            int sx1 = (int)((long)(x + 1) * im->w / nw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < im->h; sy++)
                for (int sx = sx0; sx < sx1 && sx < im->w; sx++) {
                    unsigned int c = im->px[(size_t)sy * im->w + sx];
                    r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF;
                    n++;
                }
            if (!n) n = 1;
            out[(size_t)y * nw + x] =
                ((unsigned)(r / n) << 16) | ((unsigned)(g / n) << 8) | (unsigned)(b / n);
        }
    }
    free(im->px);
    im->px = out; im->w = nw; im->h = nh;
    return 1;
}

/* --- entities -------------------------------------------------------------
 * Only the ones that actually appear in prose. Anything else is left as it
 * was written, which is better than swallowing it. */

static const struct { const char *name; const char *utf; } ENTS[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " }, { "mdash", "-" }, { "ndash", "-" },
    { "hellip", "..." }, { "ldquo", "\"" }, { "rdquo", "\"" },
    { "lsquo", "'" }, { "rsquo", "'" }, { "middot", "*" }, { "bull", "*" },
    { "copy", "(c)" }, { "reg", "(R)" }, { "trade", "(tm)" }, { "times", "x" },
    { "deg", "deg" }, { "laquo", "<<" }, { "raquo", ">>" }, { "shy", "" },
};

/* Decode the entity at s (pointing just past '&'). Returns its length in the
 * source, or 0 if it is not one. */
static int entity(const char *s, int len, unsigned short *out, int *outlen) {
    for (int i = 0; i < len && i < 12; i++) {
        if (s[i] != ';') continue;
        char name[12];
        int n = i;
        if (n <= 0 || n >= (int)sizeof name) return 0;
        for (int k = 0; k < n; k++) name[k] = s[k];
        name[n] = 0;

        if (name[0] == '#') {                       /* numeric */
            int v = 0, k = 1, hex = (name[1] == 'x' || name[1] == 'X');
            if (hex) k = 2;
            for (; name[k]; k++) {
                int d;
                if (name[k] >= '0' && name[k] <= '9') d = name[k] - '0';
                else if (hex && name[k] >= 'a' && name[k] <= 'f') d = name[k] - 'a' + 10;
                else if (hex && name[k] >= 'A' && name[k] <= 'F') d = name[k] - 'A' + 10;
                else return 0;
                v = v * (hex ? 16 : 10) + d;
            }
            *outlen = cp_fold(v, out);
            return i + 1;
        }

        for (unsigned e = 0; e < sizeof ENTS / sizeof ENTS[0]; e++) {
            if (!ci_eq(name, ENTS[e].name)) continue;
            int m = (int)strlen(ENTS[e].utf);
            for (int k = 0; k < m; k++) out[k] = (unsigned char)ENTS[e].utf[k];
            *outlen = m;
            return i + 1;
        }
        return 0;
    }
    return 0;
}

/* --- turning HTML into items ---------------------------------------------- */

/* --- the element stack -----------------------------------------------------
 * What is open right now, and what each open element computed to. Colour,
 * weight and size are inherited from the enclosing element; display and the
 * margins are not, which is exactly why they are kept apart. */

#define ESTACK_MAX 64

typedef struct {
    char        tag[24];
    css_style_t st;
    int         node;        /* which element opened this level */
    unsigned char has_bg;    /* the ground in force inside this element */
    unsigned int  bg;
} el_t;

static el_t   estack[ESTACK_MAX];
static int    edepth;
static int    hidden_at = -1;   /* depth where display:none took hold */
static css_style_t css_cur;     /* the style in force for text right now */

/* What is painted behind the text right now. A background is not inherited in
 * CSS -- it belongs to the element that set it -- but what is BEHIND a word
 * plainly is, and that is what decides whether its colour can be read. */
static unsigned char cur_has_bg;
static unsigned int  cur_bg;

static int void_tag(const char *t) {
    static const char *V[] = { "br","hr","img","input","meta","link","source",
        "area","base","col","embed","param","track","wbr", 0 };
    for (int i = 0; V[i]; i++) if (ci_eq(t, V[i])) return 1;
    return 0;
}

static void estack_reset(void) {
    edepth = 0;
    hidden_at = -1;
    css_style_init(&css_cur);
    cur_has_bg = 0;
    cur_bg = 0;
}

/* Which element the words being emitted belong to. A click lands on an item,
 * and an item has to be able to say which part of the document it is. */
static int cur_render_node = -1;

static int pending;              /* 0 none, 1 line break, 2 paragraph */
static int saw_space = 1;        /* was there whitespace since the last word? */
static int cur_link = -1;
static int cur_style = ST_TEXT;
static int cur_scale = 1;
static int pre_depth;

static void emit(int kind, int off, int len) {
    if (nitems >= MAX_ITEMS) return;
    item_t *it = &items[nitems++];
    it->kind = (unsigned char)kind;
    it->off = off; it->len = len;
    it->style = (unsigned char)cur_style;
    it->scale = (unsigned char)cur_scale;
    it->link = (short)cur_link;
    it->node = cur_render_node;
    it->glue = 0;
    it->wpx = 0;
    it->has_color = 0;
    it->color = 0;
    it->x = it->y = 0;

    if (kind == IT_WORD) {
        /* The page's own colour, but only where it would still be legible
         * against the ground it will actually sit on -- which is the page's
         * own background where it painted one, and ours where it did not. */
        it->on_bg = cur_has_bg;
        it->ground = cur_bg;
        unsigned ground = cur_has_bg ? cur_bg : GC_WIN;
        if (css_cur.has_color && cur_style != ST_LINK &&
            css_readable(css_cur.color, ground)) {
            it->has_color = 1;
            it->color = css_cur.color;
        }
        if (css_cur.weight == CSS_W_BOLD && cur_style == ST_TEXT) it->style = ST_BOLD;
        if (css_cur.weight == CSS_W_NORMAL && cur_style == ST_BOLD) it->style = ST_TEXT;
        if (css_cur.scale) it->scale = css_cur.scale;
        it->decor = css_cur.decor;
    }
}

static void flush_pending(void) {
    if (!pending) return;
    if (nitems) emit(pending == 2 ? IT_PARA : IT_BREAK, 0, 0);
    pending = 0;
    saw_space = 1;
}

/* Upper and lower case, for the alphabets this font actually draws. */
static int cp_upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;      /* Cyrillic */
    if (c == 0x451) return 0x401;                       /* yo */
    return c;
}
static int cp_lower_cp(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c == 0x401) return 0x451;
    return c;
}

static void put_word(const unsigned short *s, int len) {
    if (len <= 0) return;
    flush_pending();

    /* text-transform changes the letters themselves, so it happens here,
     * before anything measures them. */
    unsigned short cased[WORD_SPLIT * 4];
    if (css_cur.tcase == CSS_T_UPPER || css_cur.tcase == CSS_T_LOWER ||
        css_cur.tcase == CSS_T_TITLE) {
        int n = len < (int)(sizeof cased / sizeof cased[0])
              ? len : (int)(sizeof cased / sizeof cased[0]);
        for (int i = 0; i < n; i++) {
            int c = s[i];
            if (css_cur.tcase == CSS_T_UPPER) c = cp_upper(c);
            else if (css_cur.tcase == CSS_T_LOWER) c = cp_lower_cp(c);
            else c = (i == 0) ? cp_upper(c) : cp_lower_cp(c);
            cased[i] = (unsigned short)c;
        }
        s = cased;
        len = n;
    }
    int first = 1;
    while (len > 0) {
        int take = len > WORD_SPLIT ? WORD_SPLIT : len;
        if (textlen + take >= MAX_TEXT) return;
        int off = textlen;
        for (int i = 0; i < take; i++) textbuf[textlen++] = s[i];
        emit(IT_WORD, off, take);
        /* The pieces of a word split for length belong together, and so does
         * a word that followed markup with no whitespace in between. */
        if (nitems) items[nitems - 1].glue = (unsigned char)(first ? !saw_space : 1);
        first = 0;
        s += take; len -= take;
    }
    saw_space = 0;
}

/* The same, for the handful of fixed strings this file supplies itself. */
static void put_ascii(const char *s) {
    unsigned short w[64];
    int n = 0;
    while (s[n] && n < (int)(sizeof w / sizeof w[0])) { w[n] = (unsigned char)s[n]; n++; }
    put_word(w, n);
}

/* Tag classification. */
static int is_block(const char *t) {
    static const char *B[] = { "p","div","section","article","header","footer",
        "nav","main","aside","form","table","tbody","thead","tfoot","ul","ol",
        "dl","dd","dt","blockquote","pre","figure","figcaption","center",
        "h1","h2","h3","h4","h5","h6","address","fieldset","legend","noscript",
        "details","summary","picture","video","audio","canvas","iframe", 0 };
    for (int i = 0; B[i]; i++) if (ci_eq(t, B[i])) return 1;
    return 0;
}

static int heading_scale(const char *t) {
    if (ci_eq(t, "h1")) return 2;
    if (ci_eq(t, "h2")) return 2;
    if (ci_eq(t, "h3") || ci_eq(t, "h4") || ci_eq(t, "h5") || ci_eq(t, "h6")) return 1;
    return 0;
}

/* Is the attribute there at all? `checked`, `selected` and `disabled` are
 * written bare, with no value, so asking for their value finds nothing and
 * every tick on the page comes out empty. */
static int attr_present(const char *a, int alen, const char *want) {
    int wl = (int)strlen(want);
    for (int i = 0; i + wl <= alen; i++) {
        if (i && !is_space(a[i-1])) continue;
        int k = 0;
        while (k < wl) {
            int x = a[i+k], y = want[k];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != y) break;
            k++;
        }
        if (k != wl) continue;
        int j = i + wl;
        /* The name has to end here, or "check" would find "checkbox". */
        if (j >= alen) return 1;
        if (is_space(a[j]) || a[j] == '=' || a[j] == '/' || a[j] == '>') return 1;
    }
    return 0;
}

/* Pull one attribute value out of a tag's attribute text. */
static int attr_value(const char *a, int alen, const char *want, char *out, int max) {
    int wl = (int)strlen(want);
    for (int i = 0; i + wl < alen; i++) {
        if (i && !is_space(a[i-1])) continue;
        int k = 0;
        while (k < wl) {
            int x = a[i+k], y = want[k];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != y) break;
            k++;
        }
        if (k != wl) continue;
        int j = i + wl;
        while (j < alen && is_space(a[j])) j++;
        if (j >= alen || a[j] != '=') continue;
        j++;
        while (j < alen && is_space(a[j])) j++;
        int quote = 0;
        if (j < alen && (a[j] == '"' || a[j] == '\'')) quote = a[j++];
        int n = 0;
        while (j < alen && n < max - 1) {
            if (quote ? a[j] == quote : (is_space(a[j]) || a[j] == '>')) break;
            /* Entities appear in attribute values too, most often &amp;. */
            if (a[j] == '&') {
                unsigned short ebuf[8]; int elen = 0;
                int used = entity(a + j + 1, alen - j - 1, ebuf, &elen);
                if (used) {
                    /* A URL is bytes; anything outside ASCII in one is beyond
                     * what this browser does with it. */
                    for (int q = 0; q < elen && n < max - 1; q++)
                        out[n++] = (ebuf[q] < 128) ? (char)ebuf[q] : '?';
                    j += 1 + used;
                    continue;
                }
            }
            out[n++] = a[j++];
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

/* --- turning the tree into things to draw -----------------------------------
 *
 * This is the code that used to run inside the tokenizer, split at the only
 * place it can be split: what happens when an element opens, and what happens
 * when it closes. Nothing about it changed. What changed is what drives it --
 * the tree rather than the source text -- which means it can be run AGAIN,
 * after a script has altered something, without fetching the page twice.
 */

static int in_title;
static unsigned short rword[WORD_SPLIT * 4];
static int rwlen;

static void flush_word(void) {
    if (!rwlen) return;
    if (in_title) {
        /* The title is not drawn; it names the window. */
        int t = (int)strlen(page_title);
        if (t + rwlen + 1 < (int)sizeof page_title) {
            if (t) page_title[t++] = ' ';
            for (int q = 0; q < rwlen; q++)
                page_title[t++] = rword[q] < 128 ? (char)rword[q] : '?';
            page_title[t] = 0;
        }
    } else {
        put_word(rword, rwlen);
    }
    rwlen = 0;
}

/* One text node. Whitespace collapses to a single space, except inside a
 * <pre>, where the author meant it. */
static void render_text(const char *h, int len) {
    if (hidden_at >= 0) return;
    for (int i = 0; i < len; ) {
        unsigned short ebuf[8];
        int elen = 1, adv = 1;

        if (h[i] == '&') {
            int used = entity(h + i + 1, len - i - 1, ebuf, &elen);
            if (used) adv = 1 + used;
            else { ebuf[0] = '&'; elen = 1; }
        } else {
            int cp;
            adv = text_next(h + i, len - i, &cp);
            elen = cp_fold(cp, ebuf);
        }

        for (int k = 0; k < elen; k++) {
            int ch = ebuf[k];
            if (is_space(ch) && !pre_depth) {
                flush_word();
                saw_space = 1;
                continue;
            }
            if (pre_depth && ch == '\n') {
                flush_word();
                pending = 1;
                flush_pending();
                continue;
            }
            if (rwlen < (int)(sizeof rword / sizeof rword[0])) rword[rwlen++] = ch;
        }
        i += adv;
    }
    /* Finish here, while this text node's element is still the current one. */
    flush_word();
}

/* The chain of elements from the root down to one element, which is what a
 * selector like "nav ul > li a" has to be matched against. Built by walking
 * parents, since the tree already knows them. */
#define CHAIN_MAX 32
static css_elem  sel_chain[CHAIN_MAX];
static char      sel_id[CHAIN_MAX][64];
static char      sel_cls[CHAIN_MAX][192];

/* Is this element the first (or last) element child of its parent? What
 * :first-child asks. */
static int elem_is_first(int node) {
    int p = dom[node].parent;
    if (p < 0) return 1;
    for (int c = dom[p].first; c >= 0; c = dom[c].next)
        if (dom[c].kind == DOM_ELEM) return c == node;
    return 0;
}

static int elem_is_last(int node) {
    int p = dom[node].parent, last = -1;
    if (p < 0) return 1;
    for (int c = dom[p].first; c >= 0; c = dom[c].next)
        if (dom[c].kind == DOM_ELEM) last = c;
    return last == node;
}

static int build_chain(int node) {
    int up[CHAIN_MAX], n = 0;
    for (int e = node; e >= 0 && e != dom_root && n < CHAIN_MAX; e = dom[e].parent)
        if (dom[e].kind == DOM_ELEM) up[n++] = e;

    for (int i = 0; i < n; i++) {
        int e = up[n - 1 - i];                 /* the root end comes first */
        sel_id[i][0] = sel_cls[i][0] = 0;
        attr_value(dom[e].attr, dom[e].attrlen, "id", sel_id[i], sizeof sel_id[i]);
        attr_value(dom[e].attr, dom[e].attrlen, "class", sel_cls[i], sizeof sel_cls[i]);
        sel_chain[i].tag     = dom[e].tag;
        sel_chain[i].id      = sel_id[i][0] ? sel_id[i] : 0;
        sel_chain[i].cls     = sel_cls[i][0] ? sel_cls[i] : 0;
        sel_chain[i].attr    = dom[e].attr;
        sel_chain[i].attrlen = dom[e].attrlen;
        sel_chain[i].first   = (unsigned char)elem_is_first(e);
        sel_chain[i].last    = (unsigned char)elem_is_last(e);
    }
    return n;
}

/* The style an element computes to, given what encloses it.
 *
 * Three sources, in strict order of authority: the element's own style
 * attribute, then the stylesheets, then what it inherits. Keeping them apart
 * matters more than it looks. css_match treats anything already set in the
 * style it is handed as an inline declaration and refuses to overrule it -- so
 * handing it the INHERITED style, as this used to, meant that one
 * `body { color: ... }` silently disabled every colour rule on the page. Each
 * source is now computed on its own and they are combined here. */
static void style_for(int node, css_style_t *out) {
    const char *a = dom[node].attr;
    int alen = dom[node].attrlen;

    char idbuf[64], clsbuf[256], stylebuf[512];
    idbuf[0] = clsbuf[0] = 0;
    attr_value(a, alen, "id", idbuf, sizeof idbuf);
    attr_value(a, alen, "class", clsbuf, sizeof clsbuf);

    /* What the stylesheets say about this element, decided purely by
     * specificity among themselves. Matched against the whole ancestor chain,
     * so that "nav a" means what it says. */
    css_style_t sheet;
    css_style_init(&sheet);
    {
        int n = build_chain(node);
        if (n > 0) css_match_chain(sel_chain, n, &sheet);
    }

    /* What the element says about itself. */
    css_style_t own;
    css_style_init(&own);
    if (attr_value(a, alen, "style", stylebuf, sizeof stylebuf))
        css_decls(&own, stylebuf, (int)strlen(stylebuf));

    /* Colour, weight and size come down the tree; display and the margins do
     * not -- a block inside a block is not itself a block for that reason. */
    *out = css_cur;
    out->display = CSS_DISP_AUTO;
    out->mt = out->mb = -1;

    /* None of the box belongs to the parent. A background, a padding, a width
     * and a border are the element's own; only colour, weight and size come
     * down the tree. Copying css_cur brought all of them along, which is why
     * they have to be cleared before the element's own are applied. */
    out->has_bg = 0;
    out->bg = 0;
    out->ml = out->mr = -1;
    out->pt = out->pb = out->pl = out->pr = -1;
    out->w_px = out->w_pct = -1;
    out->maxw_px = out->maxw_pct = -1;
    out->bt = out->bb = out->bl = out->br = -1;
    out->has_bcolor = 0;
    out->bcolor = 0;
    out->fdir = CSS_FD_AUTO;
    out->cols = 0;
    out->grow = 0;
    out->pos = CSS_POS_AUTO;
    out->gap = -1;

    if (sheet.weight)    out->weight = sheet.weight;
    if (sheet.scale)     out->scale  = sheet.scale;
    if (sheet.align)     out->align  = sheet.align;
    if (sheet.has_color) { out->has_color = 1; out->color = sheet.color; }
    if (sheet.has_bg)    { out->has_bg = 1; out->bg = sheet.bg; }
    if (sheet.decor)     out->decor  = sheet.decor;
    if (sheet.tcase)     out->tcase  = sheet.tcase;
    if (sheet.vis)       out->vis      = sheet.vis;
    if (sheet.display)   out->display = sheet.display;
    if (sheet.mt >= 0)   out->mt = sheet.mt;
    if (sheet.mb >= 0)   out->mb = sheet.mb;
    if (sheet.ml != -1)  out->ml = sheet.ml;
    if (sheet.mr != -1)  out->mr = sheet.mr;
    if (sheet.pt >= 0)   out->pt = sheet.pt;
    if (sheet.pb >= 0)   out->pb = sheet.pb;
    if (sheet.pl >= 0)   out->pl = sheet.pl;
    if (sheet.pr >= 0)   out->pr = sheet.pr;
    if (sheet.w_px != -1 || sheet.w_pct >= 0) {
        out->w_px = sheet.w_px; out->w_pct = sheet.w_pct;
    }
    if (sheet.maxw_px != -1 || sheet.maxw_pct >= 0) {
        out->maxw_px = sheet.maxw_px; out->maxw_pct = sheet.maxw_pct;
    }
    if (sheet.bt >= 0) out->bt = sheet.bt;
    if (sheet.bb >= 0) out->bb = sheet.bb;
    if (sheet.bl >= 0) out->bl = sheet.bl;
    if (sheet.br >= 0) out->br = sheet.br;
    if (sheet.has_bcolor) { out->has_bcolor = 1; out->bcolor = sheet.bcolor; }
    if (sheet.fdir)      out->fdir = sheet.fdir;
    if (sheet.cols)      out->cols = sheet.cols;
    if (sheet.grow)      out->grow = sheet.grow;
    if (sheet.pos)       out->pos  = sheet.pos;
    if (sheet.gap >= 0)  out->gap  = sheet.gap;

    if (own.weight)    out->weight = own.weight;
    if (own.scale)     out->scale  = own.scale;
    if (own.align)     out->align  = own.align;
    if (own.has_color) { out->has_color = 1; out->color = own.color; }
    if (own.has_bg)    { out->has_bg = 1; out->bg = own.bg; }
    if (own.decor)     out->decor  = own.decor;
    if (own.tcase)     out->tcase  = own.tcase;
    if (own.vis)       out->vis    = own.vis;
    if (own.display)   out->display = own.display;
    if (own.mt >= 0)   out->mt = own.mt;
    if (own.mb >= 0)   out->mb = own.mb;
    if (own.ml != -1)  out->ml = own.ml;
    if (own.mr != -1)  out->mr = own.mr;
    if (own.pt >= 0)   out->pt = own.pt;
    if (own.pb >= 0)   out->pb = own.pb;
    if (own.pl >= 0)   out->pl = own.pl;
    if (own.pr >= 0)   out->pr = own.pr;
    if (own.w_px != -1 || own.w_pct >= 0) {
        out->w_px = own.w_px; out->w_pct = own.w_pct;
    }
    if (own.maxw_px != -1 || own.maxw_pct >= 0) {
        out->maxw_px = own.maxw_px; out->maxw_pct = own.maxw_pct;
    }
    if (own.bt >= 0) out->bt = own.bt;
    if (own.bb >= 0) out->bb = own.bb;
    if (own.bl >= 0) out->bl = own.bl;
    if (own.br >= 0) out->br = own.br;
    if (own.has_bcolor) { out->has_bcolor = 1; out->bcolor = own.bcolor; }
    if (own.fdir)      out->fdir = own.fdir;
    if (own.cols)      out->cols = own.cols;
    if (own.grow)      out->grow = own.grow;
    if (own.pos)       out->pos  = own.pos;
    if (own.gap >= 0)  out->gap  = own.gap;
}

/* The form an element sits in, or -1. Found by walking up rather than kept on
 * a stack, because a page closes its tags in whatever order it likes and a
 * stack that gets out of step submits the wrong fields. */
static int form_of(int node) {
    for (int a = dom[node].parent; a >= 0; a = dom[a].parent)
        if (dom[a].kind == DOM_ELEM && ci_eq(dom[a].tag, "form")) return a;
    return -1;
}

static void field_text(int node, char *out, int max) {
    int n = dom_text_into(node, out, max, 0);
    out[n < max ? n : max - 1] = 0;
    /* One run of spaces, and none at either end: a label is written across
     * several lines in the markup and read as one word on the page. */
    int w = 0, sp = 0;
    for (int i = 0; out[i]; i++) {
        int c = (unsigned char)out[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = w > 0; continue; }
        if (sp) { out[w++] = ' '; sp = 0; }
        out[w++] = out[i];
    }
    out[w] = 0;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Make a field out of an <input>, <textarea>, <select> or <button>, and say
 * how many characters wide it should be drawn. Returns -1 for one that has
 * nothing to draw. */
static int field_make(int node, const char *tag, const char *a, int alen) {
    if (nfields >= MAX_FIELDS) return -1;
    field_t *f = &fields[nfields];
    memset(f, 0, sizeof *f);
    f->node = node;
    f->form = form_of(node);
    f->rows = 1;
    f->opt = -1;
    attr_value(a, alen, "name", f->name, sizeof f->name);
    {
        char tmp[8];
        (void)tmp;
        f->disabled = attr_present(a, alen, "disabled");
    }

    char buf[64];
    if (ci_eq(tag, "textarea")) {
        f->kind = FLD_AREA;
        /* Not run through field_text: the line breaks in here are the value,
         * not markup whitespace. The one right after the tag is markup, and
         * the standard says to drop it. */
        int n = dom_text_into(node, f->value, sizeof f->value, 0);
        f->value[n < (int)sizeof f->value ? n : (int)sizeof f->value - 1] = 0;
        if (f->value[0] == '\n') memmove(f->value, f->value + 1, strlen(f->value));
        f->cols = attr_value(a, alen, "cols", buf, sizeof buf) ? atoi(buf) : 40;
        f->rows = attr_value(a, alen, "rows", buf, sizeof buf) ? atoi(buf) : 3;
        f->cols = (short)clampi(f->cols, 8, 72);
        f->rows = (short)clampi(f->rows, 1, 12);
    } else if (ci_eq(tag, "select")) {
        f->kind = FLD_SELECT;
        for (int c = dom[node].first; c >= 0 && f->nopt < MAX_OPTS; c = dom[c].next) {
            if (dom[c].kind != DOM_ELEM || !ci_eq(dom[c].tag, "option")) continue;
            int i = f->nopt++;
            field_text(c, f->optlab[i], sizeof f->optlab[i]);
            if (!attr_value(dom[c].attr, dom[c].attrlen, "value",
                            f->optval[i], sizeof f->optval[i]))
                snprintf(f->optval[i], sizeof f->optval[i], "%s", f->optlab[i]);
            if (attr_present(dom[c].attr, dom[c].attrlen, "selected")) f->opt = i;
        }
        if (f->opt < 0 && f->nopt > 0) f->opt = 0;
        int w = 4;
        for (int i = 0; i < f->nopt; i++) {
            int l = (int)strlen(f->optlab[i]);
            if (l > w) w = l;
        }
        f->cols = (short)clampi(w, 4, 40);
        if (f->opt >= 0) snprintf(f->value, sizeof f->value, "%s", f->optval[f->opt]);
    } else if (ci_eq(tag, "button")) {
        char ty[16];
        attr_value(a, alen, "type", ty, sizeof ty);
        f->kind = (ty[0] && !ci_eq(ty, "submit")) ? FLD_BUTTON : FLD_SUBMIT;
        field_text(node, f->label, sizeof f->label);
        if (!f->label[0]) snprintf(f->label, sizeof f->label, "button");
        f->cols = (short)clampi((int)strlen(f->label), 1, 40);
    } else {                                             /* <input> */
        char ty[24];
        if (!attr_value(a, alen, "type", ty, sizeof ty)) ty[0] = 0;
        attr_value(a, alen, "value", f->value, sizeof f->value);

        if (ci_eq(ty, "hidden"))        f->kind = FLD_HIDDEN;
        else if (ci_eq(ty, "checkbox")) f->kind = FLD_CHECK;
        else if (ci_eq(ty, "radio"))    f->kind = FLD_RADIO;
        else if (ci_eq(ty, "password")) f->kind = FLD_PASSWORD;
        else if (ci_eq(ty, "submit") || ci_eq(ty, "image")) f->kind = FLD_SUBMIT;
        else if (ci_eq(ty, "button") || ci_eq(ty, "reset")) f->kind = FLD_BUTTON;
        else                            f->kind = FLD_TEXT;

        if (f->kind == FLD_CHECK || f->kind == FLD_RADIO) {
            f->checked = attr_present(a, alen, "checked");
            if (!f->value[0]) snprintf(f->value, sizeof f->value, "on");
            f->cols = 3;
        } else if (f->kind == FLD_SUBMIT || f->kind == FLD_BUTTON) {
            snprintf(f->label, sizeof f->label, "%s",
                     f->value[0] ? f->value : "Submit");
            f->cols = (short)clampi((int)strlen(f->label), 1, 40);
        } else {
            /* A page sizes a search box for a window much wider than ours, so
             * what it asked for is a hint and not an instruction. */
            int w = attr_value(a, alen, "size", buf, sizeof buf) ? atoi(buf) : 24;
            if (w <= 0) w = 24;
            f->cols = (short)clampi(w, 6, 48);
            if (!f->value[0])
                attr_value(a, alen, "placeholder", f->value, sizeof f->value);
        }
    }
    f->cur = (int)strlen(f->value);
    return nfields++;
}

/* A textarea's value, cut into the lines it is drawn on: at every newline it
 * contains, and again wherever a line runs past the width of the box. */
static int area_lines(const field_t *f, int *start, int *len, int max) {
    int n = 0, i = 0, vl = (int)strlen(f->value);
    while (n < max) {
        int begin = i, k = 0;
        while (i < vl && f->value[i] != '\n' && k < f->cols) { i++; k++; }
        start[n] = begin;
        len[n] = k;
        n++;
        if (i < vl && f->value[i] == '\n') i++;
        else if (i >= vl) break;
    }
    return n;
}

/* How wide the drawn box is, brackets and all. */
static int field_width(const field_t *f) {
    switch (f->kind) {
    case FLD_CHECK: case FLD_RADIO:  return 3;
    case FLD_SUBMIT: case FLD_BUTTON: return f->cols + 4;
    case FLD_SELECT:                  return f->cols + 4;
    default:                          return f->cols + 2;
    }
}

/* Returns 1 if the children should be walked and close_el called afterwards. */
static int open_el(int node) {
    const char *name = dom[node].tag;
    const char *a = dom[node].attr;
    int alen = dom[node].attrlen;
    int has_children = (dom[node].first >= 0) || !void_tag(name);

    flush_word();

    /* Elements that contribute nothing to the page. Their contents are not
     * text -- a stylesheet and a script are neither -- and they are dealt with
     * before the element stack, because anything that leaves here without
     * being pushed must not be popped either. */
    if (ci_eq(name, "style") || ci_eq(name, "link") || ci_eq(name, "script") ||
        ci_eq(name, "meta") || ci_eq(name, "base"))
        return 0;

    /* --- the element stack, and what the stylesheets say about it -------- */
    if (has_children && edepth < ESTACK_MAX) {
        el_t *e = &estack[edepth];
        snprintf(e->tag, sizeof e->tag, "%s", name);
        e->node = node;
        style_for(node, &e->st);
        edepth++;
        css_cur = e->st;
        if ((css_cur.display == CSS_DISP_NONE || css_cur.vis == CSS_V_HIDDEN) &&
            hidden_at < 0)
            hidden_at = edepth - 1;
    } else {
        /* A void element still gets a style, for one tag's worth. */
        css_style_t tmp;
        style_for(node, &tmp);
        if (tmp.display == CSS_DISP_NONE || tmp.vis == CSS_V_HIDDEN) return 0;
        has_children = 0;
    }

    if (hidden_at >= 0) return has_children;

    /* The style this element computed to, kept for the layout pass. */
    node_style[node] = css_cur;
    node_i0[node] = nitems;
    node_i1[node] = nitems;

    /* What is behind the text from here down. A background is not inherited
     * in CSS, but what is BEHIND a word plainly is, and that decides whether
     * its colour can be read. */
    if (css_cur.has_bg && css_readable(css_cur.bg, cur_has_bg ? cur_bg : GC_WIN)) {
        cur_has_bg = 1;
        cur_bg = css_cur.bg;
    }
    if (has_children && edepth > 0) {
        estack[edepth - 1].has_bg = cur_has_bg;
        estack[edepth - 1].bg = cur_bg;
    }

    if (ci_eq(name, "title")) { in_title = 1; return has_children; }

    if (ci_eq(name, "br")) { pending = pending > 1 ? pending : 1; flush_pending(); return has_children; }
    if (ci_eq(name, "hr")) { pending = 0; emit(IT_RULE, 0, 0); return has_children; }

    if (ci_eq(name, "li")) { pending = 0; emit(IT_BULLET, 0, 0); return has_children; }
    /* A row and a cell used to be a line break and a vertical bar, because
     * there was nothing here that could put things in columns. There is now,
     * and if it ever declines the job these are still a box each, which reads
     * as one cell per line rather than as a run-on sentence. */
    if (ci_eq(name, "tr") || ci_eq(name, "td") || ci_eq(name, "th")) {
        /* A break left over from the block above must not fall into a
         * cell: it would push the first cell of a row down a line and
         * leave the rest of the row sitting above it. */
        pending = 0;
        return has_children;
    }

    if (ci_eq(name, "a")) {
        char href[512];
        if (attr_value(a, alen, "href", href, sizeof href) &&
            href[0] && href[0] != '#' &&
            strncmp(href, "javascript:", 11) != 0 &&
            nlinks < MAX_LINKS) {
            int hl = (int)strlen(href);
            if (hreflen + hl + 1 < MAX_HREF) {
                linkoff[nlinks] = hreflen;
                for (int k = 0; k <= hl; k++) hrefbuf[hreflen++] = href[k];
                cur_link = nlinks++;
                cur_style = ST_LINK;
            }
        }
        return has_children;
    }

    if (ci_eq(name, "b") || ci_eq(name, "strong")) { cur_style = ST_BOLD; return has_children; }
    if (ci_eq(name, "code") || ci_eq(name, "tt") || ci_eq(name, "kbd") ||
        ci_eq(name, "samp")) {
        if (cur_style != ST_LINK) cur_style = ST_CODE;
        return has_children;
    }

    if (ci_eq(name, "svg")) {
        /* An icon written out in the page. Fifteen of github.com's links hold
         * nothing but one of these, which is why they were invisible and why
         * there was nothing under the pointer to press.
         *
         * The tree kept the source; the rasteriser wants it back with its own
         * tag around it, because that is where the size and the view box are
         * written. */
        static char src[64 * 1024];
        static int drawn;
        if (nitems == 0) drawn = 0;            /* a fresh page */

        int inner = dom_text_into(node, src, (int)sizeof src, 0);
        /* Rebuilding the tag by hand rather than with dom_html_into, which
         * would escape the very characters a path is made of. */
        char head[1024];
        int hn = snprintf(head, sizeof head, "<svg %.*s>",
                          alen < (int)sizeof head - 8 ? alen : (int)sizeof head - 8,
                          a ? a : "");
        int total = hn + inner + 6;

        image_t im;
        im.px = 0; im.w = im.h = 0;
        /* A drawing has no ground of its own; it is painted onto whatever the
         * page put behind it. */
        img_background = (cur_has_bg ? cur_bg : GC_WIN) & 0xFFFFFF;
        if (drawn < 128 && total < (int)sizeof src) {
            /* The source is in `src` already; make room for the tag in front
             * of it and close it off behind. */
            memmove(src + hn, src, (size_t)inner);
            memcpy(src, head, (size_t)hn);
            memcpy(src + hn + inner, "</svg>", 6);
            svg_decode((const unsigned char *)src, (unsigned long)total, &im);
            drawn++;
        }

        /* An icon drawn out of <use> and <symbol>, which this rasteriser
         * cannot follow, comes back as a picture of nothing: the right size,
         * the right ground, and not one mark on it. That is not a failure the
         * decoder reports, so it is one to look for -- otherwise the page
         * gets a blank square where a label would have said what the thing
         * does. */
        int marked = 0;
        if (im.px && im.w > 0 && im.h > 0) {
            long n2 = (long)im.w * im.h;
            if (n2 > 65536) n2 = 65536;         /* a glance is enough */
            for (long k = 0; k < n2; k++)
                if ((im.px[k] & 0xFFFFFF) != (img_background & 0xFFFFFF)) {
                    marked = 1;
                    break;
                }
        }

        int slot = marked ? pic_inline(&im) : -1;
        if (slot >= 0) {
            /* The rasteriser draws small things larger than asked, on the
             * understanding that whoever wanted them will shrink them back --
             * so the size the page asked for is what the picture is told it
             * is. An icon with no size of its own is a line and a half tall,
             * which is what an icon beside a word should be. */
            char dim[32];
            int want_w = 0, want_h = 0;
            if (attr_value(a, alen, "width", dim, sizeof dim))  want_w = atoi(dim);
            if (attr_value(a, alen, "height", dim, sizeof dim)) want_h = atoi(dim);
            if (want_w <= 0 && want_h <= 0) {
                want_h = 20;
                want_w = im.h > 0 ? (int)((long)im.w * want_h / im.h) : want_h;
            } else if (want_w <= 0) {
                want_w = im.h > 0 ? (int)((long)im.w * want_h / im.h) : want_h;
            } else if (want_h <= 0) {
                want_h = im.w > 0 ? (int)((long)im.h * want_w / im.w) : want_w;
            }
            if (want_w > 512) want_w = 512;
            if (want_h > 512) want_h = 512;
            pics[slot].want_w = want_w;
            pics[slot].want_h = want_h;
            /* Wanting it smaller is not enough: a picture that is already
             * drawn is measured by what it is, so it is shrunk to what was
             * asked for -- which is also the sharp way round, the drawing
             * having been made oversized for exactly this. */
            pic_fit(&pics[slot].im, want_w, want_h);

            flush_pending();
            emit(IT_IMAGE, slot, 0);
            saw_space = 1;
        } else {
            /* Nothing to draw, but a link with nothing in it cannot be
             * pressed either, so it gets something to aim at. */
            img_free(&im);
            im.px = 0;
            char lab[96];
            if (attr_value(a, alen, "aria-label", lab + 1, (int)sizeof lab - 3) ||
                attr_value(a, alen, "title", lab + 1, (int)sizeof lab - 3)) {
                lab[0] = '[';
                int l = (int)strlen(lab);
                lab[l] = ']'; lab[l + 1] = 0;
                put_ascii(lab);
            }
        }
        return 0;                               /* its children are not HTML */
    }

    if (ci_eq(name, "input") || ci_eq(name, "textarea") ||
        ci_eq(name, "select") || ci_eq(name, "button")) {
        int fi = field_make(node, name, a, alen);
        if (fi >= 0 && fields[fi].kind != FLD_HIDDEN) {
            flush_pending();
            emit(IT_FIELD, fi, field_width(&fields[fi]));
            saw_space = 1;
        }
        /* Their children are not page text: a textarea's is its value, a
         * select's are its choices, a button's is the word on the button. */
        return 0;
    }

    if (ci_eq(name, "img")) {
        char src[600];
        int got = attr_value(a, alen, "src", src, sizeof src);
        /* Lazy-loaded pages keep the real address here and put a placeholder
         * in src. */
        if (!got || !src[0])
            got = attr_value(a, alen, "data-src", src, sizeof src);

        int slot = -1;
        if (got && src[0]) {
            char abs[1024];
            url_resolve(cur_url, src, abs, sizeof abs);
            slot = pic_slot(abs);
        }

        if (slot >= 0) {
            char dim[32];
            if (!pics[slot].alt[0])
                attr_value(a, alen, "alt", pics[slot].alt, sizeof pics[slot].alt);
            if (attr_value(a, alen, "width", dim, sizeof dim))
                pics[slot].want_w = atoi(dim);
            if (attr_value(a, alen, "height", dim, sizeof dim))
                pics[slot].want_h = atoi(dim);
            flush_pending();
            emit(IT_IMAGE, slot, 0);
            saw_space = 1;
        } else {
            char alt[128];
            if (attr_value(a, alen, "alt", alt, sizeof alt) && alt[0]) {
                char lab[160];
                snprintf(lab, sizeof lab, "[%s]", alt);
                put_ascii(lab);
            } else {
                put_ascii("[image]");
            }
        }
        return has_children;
    }

    if (ci_eq(name, "pre")) pre_depth++;

    {
        int hs = heading_scale(name);
        if (hs) { cur_scale = hs; cur_style = ST_HEAD; }
    }

    if (is_block(name)) pending = 2;
    return has_children;
}

static void close_el(int node) {
    const char *name = dom[node].tag;

    flush_word();
    if (node >= 0 && node < DOM_MAX_NODES) node_i1[node] = nitems;

    /* Unwind to just outside this element. Matched on the node rather than
     * the tag name: <div><div></div></div> would otherwise close the outer one
     * first and lose a level every time. */
    for (int d = edepth - 1; d >= 0; d--) {
        if (estack[d].node == node) {
            edepth = d;
            if (hidden_at >= 0 && edepth <= hidden_at) hidden_at = -1;
            if (edepth > 0) {
                css_cur = estack[edepth - 1].st;
                cur_has_bg = estack[edepth - 1].has_bg;
                cur_bg = estack[edepth - 1].bg;
            } else {
                css_style_init(&css_cur);
                cur_has_bg = 0;
                cur_bg = 0;
            }
            break;
        }
    }

    if (hidden_at >= 0) return;

    if (ci_eq(name, "title")) { in_title = 0; return; }
    if (ci_eq(name, "li")) { pending = pending > 1 ? pending : 1; return; }
    if (ci_eq(name, "a")) {
        cur_link = -1;
        if (cur_style == ST_LINK) cur_style = ST_TEXT;
        return;
    }
    if (ci_eq(name, "b") || ci_eq(name, "strong")) { cur_style = ST_TEXT; return; }
    if (ci_eq(name, "code") || ci_eq(name, "tt") || ci_eq(name, "kbd") ||
        ci_eq(name, "samp")) {
        if (cur_style != ST_LINK) cur_style = ST_TEXT;
        return;
    }
    if (ci_eq(name, "pre")) { if (pre_depth) pre_depth--; return; }

    if (heading_scale(name)) { cur_scale = 1; cur_style = ST_TEXT; }
    if (is_block(name)) pending = 2;
}

static void render_node(int node) {
    if (node < 0) return;
    int saved = cur_render_node;
    if (dom[node].kind == DOM_TEXT) {
        cur_render_node = dom[node].parent;
        render_text(dom[node].text, dom[node].textlen);
        cur_render_node = saved;
        return;
    }
    cur_render_node = node;
    if (open_el(node)) {
        for (int c = dom[node].first; c >= 0; c = dom[c].next) render_node(c);
        cur_render_node = node;
        close_el(node);
    }
    cur_render_node = saved;
}

/* Every stylesheet the page carries, gathered before anything is laid out.
 * The old single pass could only apply a stylesheet to what came after it;
 * doing it here is both simpler and what the cascade actually says. */
/* The page's root element, as the cascade sees it. A site defines one set of
 * theme variables per theme and guards each with a selector on <html>, so this
 * is what chooses between them. */
static int root_element(css_elem *out) {
    static char rid[64], rcls[192];
    for (int i = 0; i < dom_n; i++) {
        if (dom[i].kind != DOM_ELEM) continue;
        if (!ci_eq(dom[i].tag, "html")) continue;
        rid[0] = rcls[0] = 0;
        attr_value(dom[i].attr, dom[i].attrlen, "id", rid, sizeof rid);
        attr_value(dom[i].attr, dom[i].attrlen, "class", rcls, sizeof rcls);
        memset(out, 0, sizeof *out);
        out->tag = dom[i].tag;
        out->id = rid[0] ? rid : 0;
        out->cls = rcls[0] ? rcls : 0;
        out->attr = dom[i].attr;
        out->attrlen = dom[i].attrlen;
        out->first = out->last = 1;
        return 1;
    }
    return 0;
}

static void collect_styles(void) {
    for (int i = 0; i < dom_n; i++) {
        if (dom[i].kind != DOM_ELEM) continue;
        if (ci_eq(dom[i].tag, "style")) {
            int c = dom[i].first;
            if (c >= 0 && dom[c].kind == DOM_TEXT)
                css_add_source(dom[c].text, dom[c].textlen);
        } else if (ci_eq(dom[i].tag, "link") && nsheets < MAX_SHEETS) {
            char rel[64], href[600];
            if (attr_value(dom[i].attr, dom[i].attrlen, "rel", rel, sizeof rel) &&
                attr_value(dom[i].attr, dom[i].attrlen, "href", href, sizeof href)) {
                int is_css = 0;
                for (int k = 0; rel[k]; k++)
                    if ((rel[k] == 's' || rel[k] == 'S') &&
                        ci_eq(rel + k, "stylesheet")) { is_css = 1; break; }
                if (is_css && href[0]) {
                    char abs[1024];
                    url_resolve(cur_url, href, abs, sizeof abs);
                    unsigned f0 = uptime_ms();
                    int n = fetch_cached(abs, (char *)dlbuf, MAX_IMG_BYTES);
                    page_css_fetch_ms += (int)(uptime_ms() - f0);
                    if (n > 0) css_add_source((const char *)dlbuf, n);
                    nsheets++;
                }
            }
        }
    }
}

/* --- what the page's own code sees ------------------------------------------
 *
 * The console goes into the same panel as the network log: a script's output
 * and the requests it caused are the two halves of the same question, and a
 * browser that hides either is harder to work with than one that shows both.
 */
#define CONSOLE_LINES 64
static char console_log[CONSOLE_LINES][200];
static int  console_n;          /* how many have ever been written */

static void web_console(const char *line) {
    snprintf(console_log[console_n % CONSOLE_LINES],
             sizeof console_log[0], "%s", line);
    console_n++;
}

static unsigned web_now(void) { return uptime_ms(); }
static double   web_clock(void) { return (double)uptime_ms(); }

/* Walk the tree and produce the drawing list. Safe to call again at any time,
 * which is what a script changing the page comes down to. */
static void render_dom(void) {
    /* A script changing the page rebuilds the drawing list, and the fields
     * along with it. What somebody typed has to survive that: the markup only
     * ever says what a field started as, and re-reading it would put the
     * search box back to empty every time the page stirred. */
    static field_t was[MAX_FIELDS];
    int nwas = nfields;
    memcpy(was, fields, sizeof(field_t) * (size_t)(nwas < MAX_FIELDS ? nwas : MAX_FIELDS));
    int focus_node = (focus_field >= 0 && focus_field < nwas)
                   ? was[focus_field].node : -1;
    nfields = 0;
    focus_field = -1;

    estack_reset();
    nitems = textlen = nlinks = hreflen = 0;
    pending = 0; cur_link = -1; cur_style = ST_TEXT; cur_scale = 1; pre_depth = 0;
    saw_space = 1; rwlen = 0; in_title = 0;
    cur_render_node = -1;
    nboxes = 0;
    for (int i = 0; i < dom_n && i < DOM_MAX_NODES; i++) {
        node_i0[i] = -1;
        node_i1[i] = 0;
    }
    page_title[0] = 0;

    /* The matches point at items that are about to be rebuilt. */
    find_nspans = find_nmatch = 0;

    items_laid_out = 0;
    if (dom_root >= 0)
        for (int c = dom[dom_root].first; c >= 0; c = dom[c].next) render_node(c);
    flush_word();

    /* Drop bullets that ended up with nothing beside them. */
    int out = 0;
    for (int i = 0; i < nitems; i++) {
        if (items[i].kind == IT_BULLET) {
            int j = i + 1;
            while (j < nitems && items[j].kind == IT_BREAK) j++;
            if (j >= nitems ||
                (items[j].kind != IT_WORD && items[j].kind != IT_FIELD))
                continue;
        }
        items[out++] = items[i];
    }
    nitems = out;

    /* Put back what was typed, matching by the element rather than by
     * position: a script that adds a row above a form must not shift every
     * value up one. */
    for (int i = 0; i < nfields; i++) {
        for (int k = 0; k < nwas; k++) {
            if (was[k].node != fields[i].node) continue;
            if (was[k].kind != fields[i].kind) break;
            memcpy(fields[i].value, was[k].value, sizeof fields[i].value);
            fields[i].checked = was[k].checked;
            fields[i].opt = was[k].opt;
            fields[i].cur = was[k].cur;
            break;
        }
        if (focus_node >= 0 && fields[i].node == focus_node) focus_field = i;
    }
}

/* Every script the page carries, run in order once the whole tree exists.
 *
 * That is the behaviour of a deferred script rather than an inline one: code
 * written to run while the page is still arriving sees a finished document
 * instead of half of one. The difference matters only to pages that measure
 * how much has loaded, and it means a script can never refer to something the
 * parser has not reached yet -- which is the far more common bug.
 */
#define MAX_SCRIPTS 24

static int web_field_read(int node, const char *what, char *out, int max);
static int web_field_write(int node, const char *what, const char *v);

static void run_scripts(void) {
    js_reset();
    js_console_sink = web_console;
    /* Defined further down, next to the rest of the field handling. */
    jsd_field_read  = web_field_read;
    jsd_field_write = web_field_write;
    js_clock_ms = web_clock;
    jsd_now_hook = web_now;
    jsd_setup(cur_url);

    /* A page is allowed to be slow; a browser is not allowed to stop
     * answering. Reading and running each script is bounded inside the engine;
     * these are the bounds on the page as a whole -- how many scripts, how
     * large, how long in total, and how much of the arena is left. Without
     * them a page with nine megabyte-sized bundles would spend a minute
     * failing to parse them. */
#define SCRIPT_MAX_BYTES  (256 * 1024)
#define SCRIPT_TOTAL_MS   6000
#define SCRIPT_ARENA_STOP (12 * 1024 * 1024)

    unsigned started = uptime_ms();
    int ran = 0, skipped = 0, failed = 0;

    /* Scripts on one page come out of one build, written against one set of
     * language features. Once several in a row have turned out to be beyond
     * this engine, the rest will be too -- and the expensive ones are the
     * external bundles, which cost a fetch of several hundred kilobytes each
     * before they can even be refused. Inline scripts keep being tried: they
     * are already here, they are often hand-written, and they cost nothing. */
#define SCRIPT_GIVE_UP 3
    int in_a_row = 0;

    for (int i = 0; i < dom_n && ran < MAX_SCRIPTS; i++) {
        if (dom[i].kind != DOM_ELEM || !ci_eq(dom[i].tag, "script")) continue;

        if (uptime_ms() - started > SCRIPT_TOTAL_MS ||
            js_memory_used() > SCRIPT_ARENA_STOP) {
            skipped++;
            continue;
        }

        char type[64];
        if (attr_value(dom[i].attr, dom[i].attrlen, "type", type, sizeof type) &&
            type[0] && !ci_eq(type, "text/javascript") &&
            !ci_eq(type, "application/javascript") && !ci_eq(type, "module")) {
            continue;            /* a data block, not code */
        }

        const char *code = 0;
        int len = 0;

        char src[600];
        if (attr_value(dom[i].attr, dom[i].attrlen, "src", src, sizeof src) && src[0]) {
            if (in_a_row >= SCRIPT_GIVE_UP) { skipped++; continue; }
            char abs[1024];
            url_resolve(cur_url, src, abs, sizeof abs);
            int n = fetch_cached(abs, (char *)dlbuf, MAX_IMG_BYTES);
            if (n <= 0) continue;
            code = (const char *)dlbuf;
            len = n;
        } else {
            int c = dom[i].first;
            if (c < 0 || dom[c].kind != DOM_TEXT || dom[c].textlen <= 0) continue;
            code = dom[c].text;
            len = dom[c].textlen;
        }

        if (len > SCRIPT_MAX_BYTES) { skipped++; continue; }

        ran++;
        if (!js_run(code, len)) {
            failed++;
            in_a_row++;
            if (js_error()) web_console(js_error());
        } else {
            in_a_row = 0;
        }
    }

    if (ran || skipped) {
        char note[160];
        snprintf(note, sizeof note,
                 "%d script%s run, %d failed, %d skipped, %u ms",
                 ran, ran == 1 ? "" : "s", failed, skipped,
                 uptime_ms() - started);
        web_console(note);
    }

    /* A great deal of page code waits for these rather than running outright. */
    jsd_fire_global("DOMContentLoaded");
    jsd_fire_global("load");
}

/* Pull `charset=x` out of wherever it was written -- a Content-Type header
 * or a meta tag, which use the same spelling for it. */
static int charset_in(const char *s, int len, char *out, int max) {
    out[0] = 0;
    for (int i = 0; i + 8 < len; i++) {
        int m = 0;
        const char *want = "charset";
        while (m < 7) {
            int x = s[i + m];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != want[m]) break;
            m++;
        }
        if (m != 7) continue;
        int k = i + 7;
        while (k < len && (s[k] == ' ' || s[k] == '=' || s[k] == '"' ||
                           s[k] == '\'')) k++;
        int q = 0;
        while (k < len && q < max - 1 && s[k] != '"' && s[k] != '\'' &&
               s[k] != ';' && s[k] != ' ' && s[k] != '>' && s[k] != '\r' &&
               s[k] != '\n' && s[k] != '/')
            out[q++] = s[k++];
        out[q] = 0;
        return q;
    }
    return 0;
}

static void parse_html(const char *h, int len) {
    unsigned t0 = uptime_ms();

    /* What this page is written in. The header is believed first, because it
     * is what the server actually knows; a meta tag is the page's own claim
     * and is only reached if the header said nothing. Either way the tag sits
     * in the first couple of kilobytes -- that is the point of it. */
    {
        char name[32];
        page_enc = ENC_UTF8;
        if (charset_in(last_ctype, (int)strlen(last_ctype), name, (int)sizeof name))
            page_enc = enc_by_name(name);
        else {
            int look = len < 4096 ? len : 4096;
            if (charset_in(h, look, name, (int)sizeof name))
                page_enc = enc_by_name(name);
        }
    }
    page_css_fetch_ms = 0;
    pics_clear();
    dom_parse(h, len);
    unsigned t1 = uptime_ms();
    collect_styles();
    {
        /* Every sheet is in hand now, so the variables can be resolved and
         * the rules read. Doing it earlier would mean parsing values whose
         * names do not stand for anything yet. */
        css_elem root;
        int n = root_element(&root);
        css_dark = (css_luma(GC_WIN) < 128);
        /* Which rules survive depends on how wide the window is, and this is
         * where that is decided. Left to the layout to set, it was still zero
         * here -- so every rule a modern sheet guards with a breakpoint was
         * thrown away, and what a page reads like at 1200 pixels was decided
         * as though the window were nothing at all wide. */
        css_viewport_w = G ? G->w : 1024;
        css_build(n ? &root : 0, n);
    }
    unsigned t2 = uptime_ms();
    render_dom();
    unsigned t3 = uptime_ms();

    /* Kept for the summary line rather than the console, because the console
     * scrolls and this is a fact about the page as a whole. */
    page_elems = dom_n;
    page_tree_ms = t1 - t0;
    page_style_ms = (int)(t2 - t1) - page_css_fetch_ms;
    page_render_ms = t3 - t2;
}

/* --- layout ---------------------------------------------------------------
 * Everything is measured in character cells, because the font is monospace.
 * A line takes the height of the tallest thing on it, and since headings are
 * block elements they always get lines of their own. */

/* How much room a picture asks for. Before it arrives that is whatever the
 * markup claimed, or a modest placeholder if it claimed nothing. */
static void pic_box(gui_t *g, int slot, int width, int *w, int *h) {
    pic_t *p = &pics[slot];
    if (p->state == PIC_OK && p->im.px) {
        *w = p->im.w; *h = p->im.h;
    } else if (p->want_w > 0 && p->want_h > 0) {
        *w = p->want_w; *h = p->want_h;
    } else {
        *w = g->fw * 12; *h = g->fh * 2;
    }
    if (*w > width) {
        if (*w > 0) *h = (int)((long)*h * width / *w);
        *w = width;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* --- layout ----------------------------------------------------------------
 *
 * A page is a column of boxes, and inside each box a flow of words.
 *
 * That is a change of kind from what this used to do, which was one flow of
 * words for the whole page. The flow is still here -- it is what happens
 * inside a box -- but a block element now gets a box of its own with a
 * position, a width, padding and a border, laid out inside whatever encloses
 * it. Without that there is nowhere for a background to go, no meaning to a
 * width, and no way for a page to show any structure at all.
 *
 * The drawing items are still one flat list in document order, which is what
 * makes this affordable: a subtree's items are always a contiguous run, so an
 * element can be laid out by placing its own run and recursing into its block
 * children.
 */

/* Does this element get a box of its own, or does it flow with the text? */
static int block_element(int node) {
    const css_style_t *st = &node_style[node];
    if (st->display == CSS_DISP_INLINE) return 0;
    if (st->display == CSS_DISP_BLOCK) return 1;
    if (st->display == CSS_DISP_FLEX || st->display == CSS_DISP_GRID) return 1;
    const char *t = dom[node].tag;
    if (is_block(t) || heading_scale(t) ||
        ci_eq(t, "li") || ci_eq(t, "body") || ci_eq(t, "html") ||
        ci_eq(t, "tr") || ci_eq(t, "td") || ci_eq(t, "th")) return 1;

    /* A tag we have never heard of -- <react-partial>, <turbo-frame>, the
     * dozen custom elements a page wraps its sections in -- is inline by the
     * letter of the standard, and a real browser still lays out the blocks
     * inside it, by splitting the inline box around them.
     *
     * We cannot split. So a box holding a block has to be a block itself:
     * otherwise the layout stops descending at this element and everything
     * under it flows as one run of words. That is one custom element in
     * github.com's header and the entire header below it -- the navigation,
     * every menu, the sign-in buttons -- printed as a column of sentences.
     *
     * One level of children is enough to tell, and it costs one scan of
     * an element we would otherwise have walked into anyway. */
    for (int c = dom[node].first; c >= 0; c = dom[c].next) {
        if (dom[c].kind != DOM_ELEM || node_i0[c] < 0) continue;
        const css_style_t *cs = &node_style[c];
        if (cs->display == CSS_DISP_INLINE) continue;
        if (cs->display == CSS_DISP_BLOCK || cs->display == CSS_DISP_FLEX ||
            cs->display == CSS_DISP_GRID) return 1;
        const char *ct = dom[c].tag;
        if (is_block(ct) || heading_scale(ct) || ci_eq(ct, "li")) return 1;
    }
    return 0;
}

/* A box the page took out of the flow, holding nothing anyone can read.
 *
 * We cannot put such a box where the page meant it to go; there is no
 * coordinate system here to put it in. So the only question is whether it
 * should still take up room in the column, and the answer depends on what is
 * inside it. Words are worth reading wherever they land, so a positioned box
 * with words stays where it falls.
 *
 * A positioned box with no words and no pictures is a gradient, a scrim, a
 * blurred shape behind something -- decoration we could not draw in any case.
 * Left in the column it is a hole the height of a thing that was never there.
 *
 * Counted on github.com: fifty-one positioned elements, thirty-six of them
 * outermost, and twenty-one of those carrying nothing at all.
 */
static int hollow_out_of_flow(int node) {
    const css_style_t *st = &node_style[node];
    if (st->pos != CSS_POS_ABSOLUTE && st->pos != CSS_POS_FIXED) return 0;
    for (int i = node_i0[node]; i < node_i1[node] && i < nitems; i++)
        if (items[i].kind == IT_WORD || items[i].kind == IT_IMAGE) return 0;
    return 1;
}

/* One flow of words, inside a box that starts at x0 and is `width` wide.
 * This is what the whole page used to be. */
static int layout_inline(gui_t *g, int i0, int i1, int x0, int width, int y) {
    int cw = g->fw, ch = g->fh;
    int x = 0, line_h = ch, line_scale = 1;
    int anything_on_line = 0;
    if (width < cw) width = cw;

    for (int i = i0; i < i1 && i < nitems; i++) {
        item_t *it = &items[i];

        switch (it->kind) {
        case IT_BREAK:
            if (anything_on_line) y += line_h;
            x = 0; line_h = ch; anything_on_line = 0; line_scale = 1;
            break;

        case IT_PARA:
            if (anything_on_line) y += line_h;
            y += ch / 2;
            x = 0; line_h = ch; anything_on_line = 0; line_scale = 1;
            break;

        case IT_RULE:
            if (anything_on_line) y += line_h;
            it->x = x0;
            it->y = y + ch / 3;
            it->len = width;                 /* how far the line runs */
            y += ch;
            x = 0; line_h = ch; anything_on_line = 0; line_scale = 1;
            break;

        case IT_BULLET:
            if (anything_on_line) y += line_h;
            x = 2 * cw;
            it->x = x0; it->y = y;
            line_h = ch; anything_on_line = 1; line_scale = 1;
            break;

        case IT_IMAGE: {
            int iw, ih;
            pic_box(g, it->off, width, &iw, &ih);

            /* Anything wider than half the column reads as a figure and gets
             * a line to itself; small ones flow with the text. */
            int standalone = iw > width / 2;
            if (anything_on_line && (standalone || x + iw > width)) {
                y += line_h;
                x = 0; line_h = ch; anything_on_line = 0; line_scale = 1;
            }
            it->x = x0 + x;
            it->y = y;
            it->len = ih;                    /* remembered for painting */
            it->wpx = iw;                    /* and so is the width */
            x += iw + cw;
            if (ih > line_h) line_h = ih;
            anything_on_line = 1;
            if (standalone) {
                y += line_h;
                x = 0; line_h = ch; anything_on_line = 0;
            }
            break;
        }

        case IT_FIELD: {
            field_t *f = &fields[it->off];
            int fw2 = it->len * cw;
            int fh2 = ch * (f->rows > 1 ? f->rows : 1);
            int sp = anything_on_line ? cw : 0;
            if (anything_on_line && x + sp + fw2 > width) {
                y += line_h;
                x = 0; line_h = ch; anything_on_line = 0; line_scale = 1;
                sp = 0;
            }
            it->x = x0 + x; it->y = y;
            if (sp) it->x += sp;
            x = x + sp + fw2;
            if (fh2 > line_h) line_h = fh2;
            anything_on_line = 1;
            break;
        }

        case IT_WORD: {
            int sc = it->scale ? it->scale : 1;
            int w = it->len * cw * sc;

            /* A heading cannot share a line with body text. */
            if (anything_on_line && sc != line_scale) {
                y += line_h;
                x = 0; line_h = ch; anything_on_line = 0;
            }
            int sp = (anything_on_line && !it->glue) ? cw * sc : 0;
            if (anything_on_line && x + sp + w > width) {
                y += line_h;
                x = 0; line_h = ch; anything_on_line = 0;
                sp = 0;
            }
            it->x = x0 + x + sp;
            it->y = y;
            x = x + sp + w;
            if (ch * sc > line_h) line_h = ch * sc;
            line_scale = sc;
            anything_on_line = 1;
            break;
        }
        }
    }
    if (anything_on_line) y += line_h;
    return y;
}

static int layout_block(gui_t *g, int node, int x, int avail, int y);
static int layout_row(gui_t *g, int node, int cx, int cw, int cy);
static int layout_table(gui_t *g, int node, int cx, int cw, int cy);

/* Narrower than this and a column stops being something anyone reads, so we
 * wrap instead. It is also what keeps a two-child flex wrapper around the
 * whole page from cutting the page in half: a wrapper whose halves would be
 * this thin gets laid out as a column, the way it was. */
#define ROW_MIN_CH 18

/* The children of one box: block ones get boxes, everything between them
 * flows. */
static int layout_children(gui_t *g, int node, int cx, int cw, int cy,
                           int i0, int i1) {
    /* A row, if this box asked for one and one will fit. Everything the page
     * lays out horizontally -- navigation, cards, columns of a footer --
     * arrives here; read as a column it is the whole newspaper. */
    if (ci_eq(dom[node].tag, "table")) {
        int r = layout_table(g, node, cx, cw, cy);
        if (r >= 0) return r;
    }

    const css_style_t *pst = &node_style[node];
    if ((pst->display == CSS_DISP_FLEX && pst->fdir != CSS_FD_COLUMN) ||
        pst->display == CSS_DISP_GRID) {
        int r = layout_row(g, node, cx, cw, cy);
        if (r >= 0) return r;
    }

    int cursor = i0;
    for (int c = dom[node].first; c >= 0; c = dom[c].next) {
        if (dom[c].kind != DOM_ELEM) continue;
        if (node_i0[c] < 0) continue;                  /* never drawn */
        if (!block_element(c)) continue;               /* flows with the text */
        if (node_i0[c] > cursor)
            cy = layout_inline(g, cursor, node_i0[c], cx, cw, cy);
        if (hollow_out_of_flow(c)) {
            /* Laid out all the same, so every item it owns gets a real
             * position and nothing is left drawn at a stale one -- then its
             * boxes are dropped and the column does not move. */
            int kept = nboxes;
            layout_block(g, c, cx, cw, cy);
            nboxes = kept;
        } else {
            cy = layout_block(g, c, cx, cw, cy);
        }
        if (node_i1[c] > cursor) cursor = node_i1[c];
    }
    if (i1 > cursor) cy = layout_inline(g, cursor, i1, cx, cw, cy);
    return cy;
}

/* --- tables ----------------------------------------------------------------
 *
 * A table was a run of text with vertical bars in it: no columns, nothing
 * lined up, and a row of numbers indistinguishable from a sentence. Half of
 * every reference page and most of an encyclopedia is written this way.
 *
 * The widths are measured rather than guessed. Each cell has two of them --
 * the longest single word in it, below which it cannot go, and the width it
 * would take if it never wrapped -- and a column takes the largest of each
 * over its cells. If every column can have what it wants, it gets it; if not,
 * the shortfall comes off the columns with the most to give.
 */
#define TBL_MAX_COLS 24
#define TBL_MAX_ROWS 512

/* Every <tr> under this table, in the order they are read. Pages put them
 * inside thead, tbody and tfoot, or inside nothing at all, so this looks for
 * the rows and not for the wrappers -- but stops at a table inside a table,
 * whose rows are its own. */
static int table_rows(int node, int *out, int max, int n) {
    for (int c = dom[node].first; c >= 0 && n < max; c = dom[c].next) {
        if (dom[c].kind != DOM_ELEM || node_i0[c] < 0) continue;
        if (ci_eq(dom[c].tag, "table")) continue;
        if (ci_eq(dom[c].tag, "tr")) { out[n++] = c; continue; }
        n = table_rows(c, out, max, n);
    }
    return n;
}

/* How many columns a cell covers. */
static int cell_span(int node) {
    char v[16];
    if (!attr_value(dom[node].attr, dom[node].attrlen, "colspan", v, sizeof v))
        return 1;
    int n = atoi(v);
    return (n >= 1 && n <= TBL_MAX_COLS) ? n : 1;
}

/* The narrowest this cell can be drawn, and the widest it would like to be.
 * Both in pixels. */
static void cell_wants(gui_t *g, int node, int *lo, int *hi) {
    int longest = 0, line = 0, widest = 0;
    for (int i = node_i0[node]; i < node_i1[node] && i < nitems; i++) {
        item_t *it = &items[i];
        if (it->kind == IT_BREAK || it->kind == IT_PARA) {
            if (line > widest) widest = line;
            line = 0;
            continue;
        }
        int w = 0;
        if (it->kind == IT_WORD) {
            w = it->len * g->fw * (it->scale ? it->scale : 1);
        } else if (it->kind == IT_IMAGE) {
            int iw, ih;
            pic_box(g, it->off, g->w, &iw, &ih);
            w = iw;
        } else if (it->kind == IT_FIELD) {
            w = it->len * g->fw;
        } else continue;

        if (w > longest) longest = w;
        line += w + (line ? g->fw : 0);
    }
    if (line > widest) widest = line;
    *lo = longest;
    *hi = widest;
}

static int layout_block(gui_t *g, int node, int x, int avail, int y);

/* Returns where the table ends, or -1 if this is not one we can lay out and
 * the caller should carry on as it would have. */
static int layout_table(gui_t *g, int node, int cx, int cw, int cy) {
    static int rows[TBL_MAX_ROWS];
    int nrows = table_rows(node, rows, TBL_MAX_ROWS, 0);
    if (nrows < 1) return -1;

    int lo[TBL_MAX_COLS], hi[TBL_MAX_COLS], ncols = 0;
    for (int c = 0; c < TBL_MAX_COLS; c++) lo[c] = hi[c] = 0;

    /* What each column wants. A cell that covers several columns says
     * nothing about any one of them, so it is left out of the measuring. */
    for (int r = 0; r < nrows; r++) {
        int col = 0;
        for (int c = dom[rows[r]].first; c >= 0 && col < TBL_MAX_COLS; c = dom[c].next) {
            if (dom[c].kind != DOM_ELEM || node_i0[c] < 0) continue;
            if (!ci_eq(dom[c].tag, "td") && !ci_eq(dom[c].tag, "th")) continue;
            int span = cell_span(c);
            if (span == 1) {
                int a, b;
                cell_wants(g, c, &a, &b);
                if (a > lo[col]) lo[col] = a;
                if (b > hi[col]) hi[col] = b;
            }
            col += span;
            if (col > ncols) ncols = col;
        }
    }
    if (ncols < 1) return -1;
    if (ncols > TBL_MAX_COLS) ncols = TBL_MAX_COLS;

    /* A cell needs room for its widest word and a space either side of it. */
    int pad = g->fw;
    int want = 0, need = 0;
    for (int c = 0; c < ncols; c++) {
        lo[c] += 2 * pad;
        hi[c] += 2 * pad;
        if (lo[c] < 3 * g->fw) lo[c] = 3 * g->fw;
        if (hi[c] < lo[c]) hi[c] = lo[c];
        want += hi[c];
        need += lo[c];
    }

    /* Even at their narrowest the columns do not fit. Squeezing them anyway
     * turns a fifteen-column comparison into fifteen vertical strips one
     * letter wide, which is worse than not making a table at all -- so this
     * hands the job back, and the cells come out one to a line. Long, but
     * every word of it is readable. */
    if (need > cw) return -1;

    int w[TBL_MAX_COLS];
    if (want <= cw) {
        for (int c = 0; c < ncols; c++) w[c] = hi[c];
    } else {
        /* The shortfall comes off the columns with the most to give. */
        int slack = want - need, take = want - cw;
        for (int c = 0; c < ncols; c++) {
            int give = hi[c] - lo[c];
            w[c] = hi[c] - (slack > 0 ? (int)((long long)take * give / slack) : 0);
            if (w[c] < lo[c]) w[c] = lo[c];
        }
    }

    /* Where each column starts. */
    int x[TBL_MAX_COLS + 1];
    x[0] = cx;
    for (int c = 0; c < ncols; c++) x[c + 1] = x[c] + w[c];
    int right = x[ncols];

    int top = cy, y = cy;
    int line_at[TBL_MAX_ROWS + 1];
    line_at[0] = y;

    for (int r = 0; r < nrows; r++) {
        int col = 0, bottom = y;
        for (int c = dom[rows[r]].first; c >= 0 && col < ncols; c = dom[c].next) {
            if (dom[c].kind != DOM_ELEM || node_i0[c] < 0) continue;
            if (!ci_eq(dom[c].tag, "td") && !ci_eq(dom[c].tag, "th")) continue;
            int span = cell_span(c);
            if (col + span > ncols) span = ncols - col;
            int cellw = x[col + span] - x[col];
            int b = layout_block(g, c, x[col] + pad, cellw - 2 * pad, y);
            if (b > bottom) bottom = b;
            col += span;
        }
        y = bottom > y ? bottom : y + g->fh;
        line_at[r + 1] = y;
    }

    /* The grid, as thin filled boxes: one per line rather than one per cell,
     * which is a few dozen instead of a few hundred. */
    if (ncols > 1 || nrows > 1) {
        for (int r = 0; r <= nrows && nboxes < MAX_BOX; r++) {
            box_t *b = &boxes[nboxes++];
            b->x = cx; b->y = line_at[r]; b->w = right - cx; b->h = 1;
            b->has_bg = 1; b->bg = GC_LINE;
            b->bt = b->bb = b->bl = b->br = 0;
            b->bcolor = GC_LINE;
        }
        for (int c = 0; c <= ncols && nboxes < MAX_BOX; c++) {
            box_t *b = &boxes[nboxes++];
            b->x = x[c]; b->y = top; b->w = 1; b->h = y - top;
            b->has_bg = 1; b->bg = GC_LINE;
            b->bt = b->bb = b->bl = b->br = 0;
            b->bcolor = GC_LINE;
        }
    }
    return y + g->fh / 2;
}

/* The children of a flex or grid box, side by side.
 *
 * Every element child is an item, inline ones included, because that is what
 * a flex container does to its children -- a row of links is a row of links
 * whether or not the tags in it are block tags.
 *
 * There is no attempt at real flex sizing. The columns are equal, they wrap
 * when there are more items than fit, and a grid is told how many it wants.
 * Equal columns are wrong for a sidebar beside an article; they are right for
 * the cards, navigation and footers that are almost all of what flex is used
 * for, and either way they are a page instead of a column.
 *
 * Returns -1 when a row is not worth making, and the caller lays the children
 * out downwards as before.
 */
static int layout_row(gui_t *g, int node, int cx, int cw, int cy) {
    const css_style_t *st = &node_style[node];

    /* The children that will stand in the row. */
    static int kid[64];
    int nk = 0;
    for (int c = dom[node].first; c >= 0 && nk < 64; c = dom[c].next)
        if (dom[c].kind == DOM_ELEM && node_i0[c] >= 0 &&
            node_i1[c] > node_i0[c] && !hollow_out_of_flow(c))
            kid[nk++] = c;
    if (nk < 2) return -1;                 /* a wrapper, not a row */

    int gap = st->gap >= 0 ? st->gap : g->fw;

    /* A grid told how many columns it wants gets them, equal and wrapping:
     * that is what a grid means. Everything else is measured. */
    if (st->display == CSS_DISP_GRID && st->cols > 1) {
        int cols = st->cols;
        int fit = (cw + gap) / (ROW_MIN_CH * g->fw + gap);
        if (cols > fit) cols = fit;
        if (cols > nk)  cols = nk;
        if (cols < 2)   return -1;
        int colw = (cw - gap * (cols - 1)) / cols;
        if (colw < g->fw) return -1;

        int j = 0, rowtop = cy, bottom = cy;
        for (int k = 0; k < nk; k++) {
            int b = layout_block(g, kid[k], cx + j * (colw + gap), colw, rowtop);
            if (b > bottom) bottom = b;
            if (++j == cols) { j = 0; rowtop = bottom; }
        }
        return bottom;
    }

    /* What each child would take if nothing wrapped, and the least it can
     * live with. The same two numbers a table column is sized by, asked of
     * the same measuring: a row of cards and a row of table cells are the
     * same problem. */
    static int lo[64], hi[64], w[64];
    int want = 0, need = 0, grow_total = 0;
    for (int k = 0; k < nk; k++) {
        cell_wants(g, kid[k], &lo[k], &hi[k]);
        const css_style_t *cs = &node_style[kid[k]];

        /* A width the page asked for outright is not a guess. */
        if (cs->w_px > 0)       hi[k] = lo[k] = cs->w_px;
        else if (cs->w_pct > 0) hi[k] = lo[k] = cw * cs->w_pct / 100;

        if (lo[k] < ROW_MIN_CH * g->fw / 2) lo[k] = ROW_MIN_CH * g->fw / 2;
        if (hi[k] < lo[k]) hi[k] = lo[k];
        if (hi[k] > cw)    hi[k] = cw;
        want += hi[k];
        need += lo[k];
        grow_total += cs->grow;
    }
    int gaps = gap * (nk - 1);

    if (need + gaps > cw) {
        /* Not even at their narrowest. Fall back to as many equal columns as
         * will fit, wrapping -- which is what a long strip of small things
         * wants anyway. */
        int cols = (cw + gap) / (ROW_MIN_CH * g->fw + gap);
        if (cols > nk) cols = nk;
        if (cols < 2)  return -1;
        int colw = (cw - gap * (cols - 1)) / cols;
        int j = 0, rowtop = cy, bottom = cy;
        for (int k = 0; k < nk; k++) {
            int b = layout_block(g, kid[k], cx + j * (colw + gap), colw, rowtop);
            if (b > bottom) bottom = b;
            if (++j == cols) { j = 0; rowtop = bottom; }
        }
        return bottom;
    }

    if (want + gaps <= cw) {
        /* Everyone can have what they wanted. What is left over goes to the
         * boxes that said they wanted it, and to nobody else -- that is the
         * whole of what flex-grow says, and without reading it a menu ends up
         * spread across equal sixths of the page. */
        for (int k = 0; k < nk; k++) w[k] = hi[k];
        int spare = cw - want - gaps;
        if (spare > 0 && grow_total > 0) {
            int given = 0;
            for (int k = 0; k < nk; k++) {
                int share = (int)((long long)spare * node_style[kid[k]].grow / grow_total);
                w[k] += share;
                given += share;
            }
            /* The rounding goes to the greediest, so the row ends flush. */
            for (int k = 0; k < nk && given < spare; k++)
                if (node_style[kid[k]].grow) { w[k] += spare - given; break; }
        }
    } else {
        /* Everyone can be there, but not at full width. The shortfall comes
         * off the boxes with the most to give. */
        int slack = want - need, take = want + gaps - cw;
        for (int k = 0; k < nk; k++) {
            int give = hi[k] - lo[k];
            w[k] = hi[k] - (slack > 0 ? (int)((long long)take * give / slack) : 0);
            if (w[k] < lo[k]) w[k] = lo[k];
        }
    }

    int x = cx, bottom = cy;
    for (int k = 0; k < nk; k++) {
        int b = layout_block(g, kid[k], x, w[k], cy);
        if (b > bottom) bottom = b;
        x += w[k] + gap;
    }
    return bottom;
}

static int layout_block(gui_t *g, int node, int x, int avail, int y) {
    const css_style_t *st = &node_style[node];

    int ml = st->ml > 0 ? st->ml : 0;
    int mr = st->mr > 0 ? st->mr : 0;
    int mt = st->mt > 0 ? st->mt : 0;
    int mb = st->mb > 0 ? st->mb : 0;
    int pl = st->pl > 0 ? st->pl : 0;
    int pr = st->pr > 0 ? st->pr : 0;
    int pt = st->pt > 0 ? st->pt : 0;
    int pb = st->pb > 0 ? st->pb : 0;
    int bt = st->bt > 0, bb = st->bb > 0, bl = st->bl > 0, br = st->br > 0;

    int outer = avail - ml - mr;
    if (outer < g->fw) { ml = mr = 0; outer = avail; }

    int box_w = outer;
    if (st->w_px > 0)        box_w = st->w_px;
    else if (st->w_pct >= 0) box_w = avail * st->w_pct / 100;
    if (st->maxw_px > 0 && box_w > st->maxw_px) box_w = st->maxw_px;
    if (st->maxw_pct >= 0) {
        int m = avail * st->maxw_pct / 100;
        if (box_w > m) box_w = m;
    }
    if (box_w > outer) box_w = outer;

    int content_w = box_w - bl - br - pl - pr;

    /* A page sizes its columns for a window much wider than ours, so a width
     * or a padding that leaves nothing to read is a measurement meant for
     * somebody else. The text comes first. */
    if (content_w < 24 * g->fw) {
        box_w = outer;
        if (pl > 8) pl = 8;
        if (pr > 8) pr = 8;
        content_w = box_w - bl - br - pl - pr;
        if (content_w < g->fw) { pl = pr = 0; content_w = box_w - bl - br; }
        if (content_w < g->fw) content_w = g->fw;
    }

    int box_x = x + ml;
    /* margin: 0 auto -- the way a page centres its column. */
    if (st->ml == CSS_LEN_AUTO && st->mr == CSS_LEN_AUTO && box_w < avail)
        box_x = x + (avail - box_w) / 2;

    int top = y + mt;
    int cy = top + bt + pt;
    int cx = box_x + bl + pl;

    cy = layout_children(g, node, cx, content_w, cy, node_i0[node], node_i1[node]);

    int bottom = cy + pb + bb;

    if ((st->has_bg || bt || bb || bl || br) && nboxes < MAX_BOX && bottom > top) {
        box_t *b = &boxes[nboxes++];
        b->x = box_x; b->y = top;
        b->w = box_w; b->h = bottom - top;
        b->has_bg = st->has_bg;
        b->bg = st->bg;
        b->bt = (unsigned char)bt; b->bb = (unsigned char)bb;
        b->bl = (unsigned char)bl; b->br = (unsigned char)br;
        b->bcolor = st->has_bcolor ? st->bcolor : GC_LINE;
    }

    return bottom + mb;
}

/* A background on something that flows with the text -- a badge, a marked
 * phrase, a piece of code in a sentence. It has no box of its own, so it is
 * given one per line it occupies: filling the whole rectangle its words span
 * would paint over the lines between them. */
static void inline_boxes(gui_t *g) {
    for (int n = 0; n < dom_n && nboxes < MAX_BOX; n++) {
        if (dom[n].kind != DOM_ELEM) continue;
        if (node_i0[n] < 0 || node_i1[n] <= node_i0[n]) continue;
        const css_style_t *st = &node_style[n];
        if (!st->has_bg && st->bt <= 0 && st->bb <= 0 &&
            st->bl <= 0 && st->br <= 0) continue;
        if (block_element(n)) continue;                /* already has a box */
        if (hollow_out_of_flow(n)) continue;

        int line_y = -1, x0 = 0, x1 = 0, h = 0;
        for (int i = node_i0[n]; i < node_i1[n] && i < nitems; i++) {
            item_t *it = &items[i];
            if (it->kind != IT_WORD && it->kind != IT_IMAGE) continue;
            int sc = it->scale ? it->scale : 1;
            int iw, ih;
            if (it->kind == IT_IMAGE) { iw = it->wpx; ih = it->len; }
            else { iw = it->len * g->fw * sc; ih = g->fh * sc; }

            if (it->y != line_y) {
                if (line_y >= 0 && nboxes < MAX_BOX) {
                    box_t *b = &boxes[nboxes++];
                    b->x = x0 - 2; b->y = line_y; b->w = x1 - x0 + 4; b->h = h;
                    b->has_bg = st->has_bg; b->bg = st->bg;
                    b->bt = (unsigned char)(st->bt > 0); b->bb = (unsigned char)(st->bb > 0);
                    b->bl = (unsigned char)(st->bl > 0); b->br = (unsigned char)(st->br > 0);
                    b->bcolor = st->has_bcolor ? st->bcolor : GC_LINE;
                }
                line_y = it->y; x0 = it->x; x1 = it->x + iw; h = ih;
            } else {
                if (it->x < x0) x0 = it->x;
                if (it->x + iw > x1) x1 = it->x + iw;
                if (ih > h) h = ih;
            }
        }
        if (line_y >= 0 && nboxes < MAX_BOX) {
            box_t *b = &boxes[nboxes++];
            b->x = x0 - 2; b->y = line_y; b->w = x1 - x0 + 4; b->h = h;
            b->has_bg = st->has_bg; b->bg = st->bg;
            b->bt = (unsigned char)(st->bt > 0); b->bb = (unsigned char)(st->bb > 0);
            b->bl = (unsigned char)(st->bl > 0); b->br = (unsigned char)(st->br > 0);
            b->bcolor = st->has_bcolor ? st->bcolor : GC_LINE;
        }
    }
}

static void layout(gui_t *g, int width) {
    /* The rules were already chosen against this width, back when the
     * stylesheets were read. Kept in step here so a query asked again during
     * layout gets the same answer. Resizing the window does not re-file the
     * rules: the page has to be reloaded for a breakpoint to change sides. */
    css_viewport_w = g->w;
    nboxes = 0;

    int y = 0;
    if (dom_root >= 0)
        y = layout_children(g, dom_root, 0, width, 0, 0, nitems);

    inline_boxes(g);

    /* Laid out again, so where the matches are has moved with everything
     * else. Cheap, and the alternative is a highlight on last page's word. */
    if (find_open && find_query[0]) find_scan();

    doc_height = y;
    items_laid_out = 1;

}

static unsigned style_color(int style) {
    switch (style) {
    case ST_LINK: return GC_ACCENT;
    case ST_CODE: return GC_ACCENT2;
    case ST_HEAD: return GC_TEXT;
    default:      return GC_TEXT;
    }
}

/* A colour that can be read on this ground, when the page did not name one
 * that could be. */
static unsigned readable_on(unsigned ground) {
    if (css_readable(GC_TEXT, ground)) return GC_TEXT;
    return css_luma(ground) > 128 ? 0x101010u : 0xF0F0F0u;
}

static void draw_page(gui_t *g) {
    if (!items_laid_out) return;
    int vx = MARGIN, vy = VIEW_TOP + MARGIN / 2;
    int vw = g->w - 2 * MARGIN - 10;
    int vh = g->h - VIEW_TOP - STAT_H - MARGIN;
    if (vw <= 0 || vh <= 0) return;

    /* The boxes first: innermost last, which is the order they finished in
     * -- a child's box is recorded before its parent's, so they are painted
     * back to front by walking the list backwards. */
    for (int b = nboxes - 1; b >= 0; b--) {
        box_t *bx = &boxes[b];
        int x0 = vx + bx->x, y0 = vy + bx->y - scroll;
        int x1 = x0 + bx->w, y1 = y0 + bx->h;
        if (y1 <= vy || y0 >= vy + vh) continue;       /* off screen */

        int cx0 = x0 < vx ? vx : x0, cx1 = x1 > vx + vw ? vx + vw : x1;
        int cy0 = y0 < vy ? vy : y0, cy1 = y1 > vy + vh ? vy + vh : y1;
        if (cx1 <= cx0 || cy1 <= cy0) continue;

        if (bx->has_bg)
            gui_fill(g, cx0, cy0, cx1 - cx0, cy1 - cy0, bx->bg);

        {
            unsigned c = bx->bcolor;
            if (bx->bt && y0 >= vy)      gui_fill(g, cx0, y0, cx1 - cx0, 1, c);
            if (bx->bb && y1 <= vy + vh) gui_fill(g, cx0, y1 - 1, cx1 - cx0, 1, c);
            if (bx->bl && x0 >= vx)      gui_fill(g, x0, cy0, 1, cy1 - cy0, c);
            if (bx->br && x1 <= vx + vw) gui_fill(g, x1 - 1, cy0, 1, cy1 - cy0, c);
        }
    }

    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        int sy = it->y - scroll;
        int s = it->scale ? it->scale : 1;

        if (sy > vh) break;                    /* the rest is below the window */

        if (it->kind == IT_RULE) {
            if (sy >= 0 && sy < vh) gui_fill(g, vx, vy + sy, vw, 1, GC_LINE);
            continue;
        }
        if (it->kind == IT_BULLET) {
            if (sy >= 0 && sy < vh)
                gui_glyph(g, vx, vy + sy, '*', GC_DIM, 1);
            continue;
        }
        if (it->kind == IT_IMAGE) {
            int iw, ih;
            pic_box(g, it->off, it->wpx > 0 ? it->wpx : vw, &iw, &ih);
            if (sy + ih < 0) continue;
            int px = vx + it->x, py = vy + sy;
            pic_t *p = &pics[it->off];

            if (p->state == PIC_OK && p->im.px) {
                /* Clipped by hand rather than by the surface, so a picture
                 * hanging off the top or bottom of the window costs nothing. */
                for (int row = 0; row < ih; row++) {
                    int dy = py + row;
                    if (dy < vy || dy >= vy + vh || dy >= g->h) continue;
                    const unsigned int *src = p->im.px + (size_t)row * p->im.w;
                    unsigned int *dst = g->px + (size_t)dy * g->w + px;
                    int n = iw;
                    if (px + n > vx + vw) n = vx + vw - px;
                    if (px < 0 || n <= 0) continue;
                    for (int k = 0; k < n; k++) dst[k] = src[k];
                }
            } else {
                /* A picture we could not decode still had something to say --
                 * the page wrote what it shows in the alt attribute, and an
                 * empty grey box says nothing at all. */
                unsigned col = (p->state == PIC_FAIL) ? GC_WARN : GC_DIM;
                gui_rect(g, px, py, iw, ih, col);
                const char *lab = (p->state != PIC_FAIL) ? "..."
                                : p->alt[0] ? p->alt : "?";
                if (iw > 6 * g->fw && ih >= g->fh)
                    gui_text_clip(g, px + 4, py + (ih - g->fh) / 2, lab,
                                  col, iw - 8);
                else
                    gui_text(g, px + 2, py + (ih - g->fh) / 2, "?", col);
            }
            continue;
        }
        if (it->kind == IT_FIELD) {
            field_t *f = &fields[it->off];
            int rows = f->rows > 1 ? f->rows : 1;
            int bw = it->len * g->fw, bh = g->fh * rows;
            if (sy + bh < 0) continue;
            int px = vx + it->x, py = vy + sy;
            int on = (it->off == focus_field);
            unsigned edge = f->disabled ? GC_DIM : (on ? GC_ACCENT : GC_LINE);
            unsigned ink  = f->disabled ? GC_DIM : GC_TEXT;

            switch (f->kind) {
            case FLD_CHECK:
            case FLD_RADIO: {
                char box[4];
                int round = (f->kind == FLD_RADIO);
                box[0] = round ? '(' : '[';
                box[1] = f->checked ? (round ? 'o' : 'x') : ' ';
                box[2] = round ? ')' : ']';
                box[3] = 0;
                gui_text(g, px, py, box, f->checked ? GC_ACCENT : ink);
                break;
            }
            case FLD_SUBMIT:
            case FLD_BUTTON: {
                gui_fill(g, px, py, bw, bh, f->disabled ? GC_WIN : GC_LINE);
                gui_rect(g, px, py, bw, bh, edge);
                gui_text_clip(g, px + 2 * g->fw, py, f->label, ink, bw - 3 * g->fw);
                break;
            }
            case FLD_SELECT: {
                gui_rect(g, px, py, bw, bh, edge);
                const char *lab = (f->opt >= 0 && f->opt < f->nopt)
                                ? f->optlab[f->opt] : "";
                gui_text_clip(g, px + g->fw, py, lab, ink, bw - 3 * g->fw);
                gui_text(g, px + bw - 2 * g->fw, py, "v", GC_DIM);
                break;
            }
            case FLD_AREA: {
                gui_rect(g, px, py, bw, bh, edge);
                int st[64], ln[64];
                int nl = area_lines(f, st, ln, 64);
                /* Follow the caret down when there is more text than box. */
                int caret_line = 0;
                for (int k = 0; k < nl; k++)
                    if (f->cur >= st[k]) caret_line = k;
                int from = 0;
                if (on && caret_line >= rows) from = caret_line - rows + 1;
                for (int k = 0; k < rows && from + k < nl; k++) {
                    int L = from + k;
                    char line[80];
                    int take = ln[L] < (int)sizeof line - 1 ? ln[L] : (int)sizeof line - 1;
                    memcpy(line, f->value + st[L], (size_t)take);
                    line[take] = 0;
                    gui_text_clip(g, px + g->fw, py + k * g->fh, line, ink,
                                  f->cols * g->fw);
                    if (on && L == caret_line) {
                        int col = f->cur - st[L];
                        if (col > f->cols) col = f->cols;
                        gui_fill(g, px + g->fw + col * g->fw, py + k * g->fh,
                                 1, g->fh, GC_ACCENT);
                    }
                }
                break;
            }
            default: {
                gui_rect(g, px, py, bw, bh, edge);
                /* A password is shown as its own length and nothing more. */
                char shown[FIELD_VAL];
                const char *src = f->value;
                if (f->kind == FLD_PASSWORD) {
                    int n = (int)strlen(f->value);
                    if (n > (int)sizeof shown - 1) n = (int)sizeof shown - 1;
                    for (int k = 0; k < n; k++) shown[k] = '*';
                    shown[n] = 0;
                    src = shown;
                }
                /* Scrolled so the caret is always in the box. */
                int inner = f->cols;
                int from = 0;
                if (on && f->cur > inner - 1) from = f->cur - (inner - 1);
                int len = (int)strlen(src);
                if (from > len) from = len;
                gui_text_clip(g, px + g->fw, py, src + from, ink, inner * g->fw);
                if (on) {
                    int cx = px + g->fw + (f->cur - from) * g->fw;
                    gui_fill(g, cx, py, 1, g->fh, GC_ACCENT);
                }
                break;
            }
            }
            continue;
        }

        if (it->kind != IT_WORD) continue;

        if (sy + g->fh * s < 0) continue;

        unsigned col = it->has_color ? it->color
                     : (it->on_bg ? readable_on(it->ground)
                                  : style_color(it->style));
        /* A link keeps its own colour, unless it would vanish on the ground
         * the page painted for it. */
        if (!it->has_color && it->on_bg && it->style == ST_LINK &&
            css_readable(GC_ACCENT, it->ground))
            col = GC_ACCENT;
        if (it->link >= 0 && it->link == hover_link) col = GC_ACCENT2;

        int px = vx + it->x, py = vy + sy;
        for (int k = 0; k < it->len; k++) {
            int gx = px + k * g->fw * s;
            if (gx > vx + vw) break;
            /* A found word is painted on its own ground, and the one being
             * looked at right now on a brighter one, so that "next" means
             * something you can see. */
            unsigned gcol = col;
            if (find_nspans) {
                int m = find_hit(i, k);
                if (m >= 0) {
                    unsigned bg = (m == find_cur) ? GC_ACCENT : GC_LINE;
                    gui_fill(g, gx, py, g->fw * s, g->fh * s, bg);
                    gcol = readable_on(bg);
                }
            }
            gui_glyph(g, gx, py, textbuf[it->off + k], gcol, s);
            /* Bold is the same glyph again, one pixel over. */
            if (it->style == ST_BOLD || it->style == ST_HEAD)
                gui_glyph(g, gx + 1, py, textbuf[it->off + k], col, s);
        }
        /* Links are underlined unless the page says otherwise, and anything
         * else is underlined if the page says so. */
        int line = (it->style == ST_LINK) ? (it->decor != CSS_D_NONE)
                                          : (it->decor == CSS_D_UNDER);
        int strike = (it->decor == CSS_D_STRIKE);
        if (line || strike) {
            int w = it->len * g->fw * s;
            if (px + w > vx + vw) w = vx + vw - px;
            int ly = strike ? py + g->fh * s / 2 : py + g->fh * s - 1;
            if (w > 0) gui_fill(g, px, ly, w, 1, col);
        }
    }
}

/* Which link, if any, is under a point in the page area. */
static int link_at(gui_t *g, int mx, int my) {
    int vx = MARGIN, vy = VIEW_TOP + MARGIN / 2;
    int vh = g->h - VIEW_TOP - STAT_H - MARGIN;
    if (my < vy || my > vy + vh) return -1;

    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (it->link < 0) continue;
        int px = vx + it->x, py = vy + it->y - scroll;
        int w, h;
        if (it->kind == IT_IMAGE) {
            /* A picture is as clickable as a word. Sixteen of the seventy-four
             * links on github.com's front page have no words in them at all,
             * and this is the only kind of those that has anything to aim at. */
            pic_box(g, it->off, it->wpx > 0 ? it->wpx : g->w - 2 * MARGIN - 10,
                    &w, &h);
        } else if (it->kind == IT_WORD) {
            int s = it->scale ? it->scale : 1;
            w = it->len * g->fw * s;
            h = g->fh * s;
        } else continue;
        if (mx >= px && mx < px + w && my >= py && my < py + h) return it->link;
    }
    return -1;
}

/* Which element is under a point. A click has to reach the script that asked
 * for it, and the only thing on screen is a list of words -- so each word
 * remembers where it came from. */
static int node_at(gui_t *g, int mx, int my) {
    int vx = MARGIN, vy = VIEW_TOP + MARGIN / 2;
    int vh = g->h - VIEW_TOP - STAT_H - MARGIN;
    if (my < vy || my > vy + vh) return -1;

    /* The word actually under the pointer, and failing that the nearest one
     * to its left on the same line -- so that the spaces inside something like
     * "[ add one ]" belong to it rather than to nothing. */
    int near = -1, near_x = -1;

    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (it->kind != IT_WORD && it->kind != IT_IMAGE) continue;
        int s = it->scale ? it->scale : 1;
        int px = vx + it->x, py = vy + it->y - scroll;
        int w, h;
        if (it->kind == IT_IMAGE)
            pic_box(g, it->off, it->wpx > 0 ? it->wpx : g->w - 2 * MARGIN - 10,
                    &w, &h);
        else { w = it->len * g->fw * s; h = g->fh * s; }

        if (my < py || my >= py + h) continue;
        if (mx >= px && mx < px + w) return it->node;

        /* On the line, to the left, and closer than anything seen so far.
         * Only within a few characters: a click far out in the margin past
         * the end of a line did not mean the last word on it. */
        if (px <= mx && px > near_x && mx - (px + w) < 3 * g->fw) {
            near_x = px;
            near = it->node;
        }
    }
    return near;
}

/* Which field an element turned into, or -1. */
static int field_of_node(int node) {
    for (int i = 0; i < nfields; i++)
        if (fields[i].node == node) return i;
    return -1;
}

/* What a script sees when it reads input.value, and what it changes when it
 * writes one. Returns -1 for an element that is not a control at all, which
 * sends jsdom back to the attributes. */
static int web_field_read(int node, const char *what, char *out, int max) {
    int i = field_of_node(node);
    if (i < 0) return -1;
    field_t *f = &fields[i];

    if (!strcmp(what, "checked")) {
        out[0] = f->checked ? '1' : '0';
        out[1] = 0;
        return 1;
    }
    if (!strcmp(what, "selectedIndex")) {
        snprintf(out, (size_t)max, "%d", f->kind == FLD_SELECT ? f->opt : -1);
        return (int)strlen(out);
    }
    const char *v = f->value;
    if (f->kind == FLD_SELECT)
        v = (f->opt >= 0 && f->opt < f->nopt) ? f->optval[f->opt] : "";
    if (f->kind == FLD_SUBMIT || f->kind == FLD_BUTTON) v = f->label;
    snprintf(out, (size_t)max, "%s", v);
    return (int)strlen(out);
}

static int web_field_write(int node, const char *what, const char *v) {
    int i = field_of_node(node);
    if (i < 0) return 0;
    field_t *f = &fields[i];
    if (!v) v = "";

    if (!strcmp(what, "checked")) {
        /* Arrives as the string a JS value turns into. */
        f->checked = (unsigned char)!(!v[0] || !strcmp(v, "false") ||
                                      !strcmp(v, "0") || !strcmp(v, "undefined") ||
                                      !strcmp(v, "null"));
        return 1;
    }
    if (!strcmp(what, "selectedIndex")) {
        int k = atoi(v);
        if (f->kind == FLD_SELECT && k >= 0 && k < f->nopt) f->opt = k;
        return 1;
    }

    snprintf(f->value, sizeof f->value, "%s", v);
    f->cur = (int)strlen(f->value);
    if (f->kind == FLD_SELECT)
        for (int k = 0; k < f->nopt; k++)
            if (!strcmp(f->optval[k], f->value)) { f->opt = k; break; }
    return 1;
}

/* Which field is under a point, or -1. */
static int field_at(gui_t *g, int mx, int my) {
    int vx = MARGIN, vy = VIEW_TOP + MARGIN / 2;
    int vh = g->h - VIEW_TOP - STAT_H - MARGIN;
    if (my < vy || my > vy + vh) return -1;

    for (int i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (it->kind != IT_FIELD) continue;
        field_t *f = &fields[it->off];
        int bw = it->len * g->fw;
        int bh = g->fh * (f->rows > 1 ? f->rows : 1);
        int px = vx + it->x, py = vy + it->y - scroll;
        if (mx >= px && mx < px + bw && my >= py && my < py + bh) return it->off;
    }
    return -1;
}

/* The link this element sits inside, or -1. A page puts a control inside an
 * anchor to make a button that goes somewhere: eight of github.com's links
 * are shaped that way, and every one of them did nothing when pressed,
 * because the control was found first and had nothing of its own to do. */
static int link_around(int node) {
    for (int a = node; a >= 0; a = dom[a].parent) {
        if (dom[a].kind != DOM_ELEM || !ci_eq(dom[a].tag, "a")) continue;
        /* Find the link by the words under it: that is where the address was
         * written down when the page was read. */
        for (int i = node_i0[a]; i >= 0 && i < node_i1[a] && i < nitems; i++)
            if (items[i].link >= 0) return items[i].link;
    }
    return -1;
}

/* Whether a field is one you can put a caret in. */
static int field_typable(const field_t *f) {
    return f->kind == FLD_TEXT || f->kind == FLD_PASSWORD || f->kind == FLD_AREA;
}

/* Tab moves to the next one you can do anything with. */
static void field_step(int dir) {
    if (nfields <= 0) return;
    int at = focus_field < 0 ? (dir > 0 ? -1 : nfields) : focus_field;
    for (int k = 0; k < nfields; k++) {
        at += dir;
        if (at < 0) at = nfields - 1;
        if (at >= nfields) at = 0;
        field_t *f = &fields[at];
        if (f->disabled || f->kind == FLD_HIDDEN) continue;
        focus_field = at;
        return;
    }
}

/* --- fetching -------------------------------------------------------------- */

static void draw(gui_t *g);

/* Wait for one fetch, drawing while it runs.
 *
 * The kernel does the transfer a few milliseconds at a time and hands the
 * processor back between slices, so this loop is what turns that into a window
 * that still repaints and a machine that is still usable while a page loads.
 * The status line counts the kilobytes as they arrive, which is the difference
 * between something working and something hung. */
/* What the next page fetch should post, if anything. Set by a form's submit
 * and cleared by the fetch that carries it, so only the navigation the form
 * asked for is a POST and the pictures and stylesheets that follow are not. */
static char post_body[2048];
static int  post_len;

static int fetch_pump(const char *host, const char *path, int port, int tls,
                      char *buf, int max) {
    int blen = post_len;
    post_len = 0;

    char ck[1600];
    cookie_header(host, path, tls, ck, (int)sizeof ck);

    /* The slot, not a yes: zero is one of them. */
    int slot = net_fetch_begin(host, path, port, tls, 1,
                               blen ? post_body : 0, blen,
                               ck[0] ? ck : 0);
    if (slot < 0) return -1;

    unsigned last_draw = 0;
    for (;;) {
        int got = 0;
        int r = net_fetch_check(slot, &got);
        if (r != NET_FETCH_PENDING)
            return r < 0 ? -1 : net_fetch_done(slot, buf, max);

        /* Twenty-five frames a second is plenty; any more and the redrawing
         * would be taking the time back off the transfer. */
        unsigned now = uptime_ms();
        if (G && now - last_draw >= 40) {
            last_draw = now;
            if (got > 0) snprintf(status, sizeof status, "%s -- %d KB", host, got / 1024);
            else         snprintf(status, sizeof status, "%s -- connecting", host);
            if (gui_sync(G)) { draw(G); gui_present(G); }
        }
    }
}

/* Returns the body length, or -1, with the body moved to the front of `buf`.
 * Follows redirects, which is why it needs the headers and not just the body.
 * `final_url` may be NULL when the caller does not care where it ended up. */
/* One header out of a response, by name, without its trailing newline.
 * Headers are `name: value`, which is not the shape attr_value understands,
 * and this was written out by hand for Location alone -- now there are three
 * of them worth reading. */
static int header_line_from(const char *buf, int hdr_end, const char *want,
                            char *out, int max, int *from) {
    out[0] = 0;
    int wl = (int)strlen(want);
    for (int i = from ? *from : 0; i + wl < hdr_end; i++) {
        if (i && buf[i-1] != '\n') continue;
        int m = 0;
        while (m < wl) {
            int x = buf[i+m];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != want[m]) break;
            m++;
        }
        if (m != wl || buf[i+m] != ':') continue;
        int k = i + m + 1;
        while (k < hdr_end && (buf[k] == ' ' || buf[k] == '\t')) k++;
        int q = 0;
        while (k < hdr_end && buf[k] != '\r' && buf[k] != '\n' && q < max - 1)
            out[q++] = buf[k++];
        out[q] = 0;
        if (from) *from = k;
        return q;
    }
    if (from) *from = hdr_end;
    return 0;
}

static int header_line(const char *buf, int hdr_end, const char *want,
                       char *out, int max) {
    return header_line_from(buf, hdr_end, want, out, max, 0);
}
/* gzip_body_at and zlib_body_at come from inflate.h, next to the DEFLATE
 * they wrap; the picture decoder needs the same two. */

/* Put the real bytes back where the compressed ones were. Returns the new
 * length, or the old one when there was nothing to undo. */
static int decompress_body(const char *enc, char *buf, int body, int n, int max) {
    int at = -1;
    const unsigned char *p = (const unsigned char *)buf + body;
    int len = n - body;
    if (len <= 0) return n;

    if (enc[0] == 'g' || enc[0] == 'x')          /* gzip, x-gzip */
        at = gzip_body_at(p, len);
    else if (enc[0] == 'd')                      /* deflate */
        at = zlib_body_at(p, len);
    if (at < 0) return n;

    unsigned long got = 0;
    unsigned char *out = inflate_raw(p + at, (unsigned long)(len - at), &got);
    if (!out || got == 0) {
        free(out);
        snprintf(status, sizeof status, "the server compressed that in a way we could not read");
        return n;
    }
    int keep = (int)got;
    if (body + keep > max - 1) keep = max - 1 - body;
    memcpy(buf + body, out, (size_t)keep);
    free(out);
    return body + keep;
}


static int fetch_url(const char *url, char *buf, int max,
                     char *final_url, int final_max) {
    char u[1024];
    snprintf(u, sizeof u, "%s", url);

    for (int hop = 0; hop < 6; hop++) {
        char host[256], path[768];
        int port, tls;
        url_split(u, host, sizeof host, path, sizeof path, &port, &tls);
        if (!host[0]) { snprintf(status, sizeof status, "that is not a web address"); return -1; }

        snprintf(status, sizeof status, "connecting to %s ...", host);

        int n = fetch_pump(host, path, port, tls, buf, max - 1);
        if (n <= 0) {
            snprintf(status, sizeof status, "could not reach %s", host);
            return -1;
        }
        buf[n] = 0;

        /* Status line: HTTP/1.1 200 OK */
        int code = 0;
        for (int i = 0; i < n - 4 && i < 64; i++) {
            if (buf[i] == ' ') {
                for (int k = i + 1; k < n && buf[k] >= '0' && buf[k] <= '9'; k++)
                    code = code * 10 + (buf[k] - '0');
                break;
            }
        }

        int body = 0;
        for (int i = 0; i + 3 < n; i++)
            if (buf[i] == '\r' && buf[i+1] == '\n' &&
                buf[i+2] == '\r' && buf[i+3] == '\n') { body = i + 4; break; }

        if (code >= 300 && code < 400) {
            /* Where to. Headers are name: value, not the name=value shape
             * attr_value understands, so this is done by hand. */
            char loc[1024];
            header_line(buf, body, "location", loc, (int)sizeof loc);
            if (!loc[0]) {
                snprintf(status, sizeof status, "the server redirected without saying where");
                return -1;
            }
            char nxt[1024];
            url_resolve(u, loc, nxt, sizeof nxt);
            snprintf(u, sizeof u, "%s", nxt);
            continue;
        }

        /* Every Set-Cookie, not just the first: a sign-in answers with
         * several, and taking one of them is the same as taking none. */
        {
            char line[900];
            int from = 0;
            while (header_line_from(buf, body, "set-cookie", line,
                                    (int)sizeof line, &from))
                cookie_take(line, host, path, tls);
        }

        header_line(buf, body, "content-type", last_ctype, (int)sizeof last_ctype);
        header_line(buf, body, "content-disposition", last_disp, (int)sizeof last_disp);

        {
            char enc[64];
            if (header_line(buf, body, "content-encoding", enc, (int)sizeof enc)) {
                for (int i = 0; enc[i]; i++)
                    if (enc[i] >= 'A' && enc[i] <= 'Z') enc[i] += 32;
                if (enc[0] != 'i')               /* identity: already itself */
                    n = decompress_body(enc, buf, body, n, max);
            }
        }

        if (final_url) snprintf(final_url, final_max, "%s", u);

        /* Move the body to the front so the caller sees only the content. */
        int blen = n - body;
        memmove(buf, buf + body, (size_t)blen);
        if (blen < max) buf[blen] = 0;

        if (code >= 400)
            snprintf(status, sizeof status, "the server answered %d", code);
        else
            snprintf(status, sizeof status, "%d bytes", blen);
        return blen;
    }
    snprintf(status, sizeof status, "too many redirects");
    return -1;
}

/* The next picture that has not been tried yet, or -1. */
static int pic_pending(void) {
    for (int i = 0; i < npics; i++)
        if (pics[i].state == PIC_WANT) return i;
    return -1;
}

static void pic_load(gui_t *g, int i) {
    pic_t *p = &pics[i];
    /* Transparency has to be flattened onto something, and the page is what is
     * actually behind the picture. Without this a white logo -- which is most
     * of them, since sites draw them on their own dark panels -- comes out
     * white on white and vanishes. */
    img_background = GC_WIN & 0xFFFFFF;
    p->state = PIC_FAIL;                    /* until it works */

    if (strncmp(p->url, "data:", 5) == 0) {
        /* Inline pictures: only base64 is worth supporting, and only when the
         * marker is there to say so. */
        const char *c = p->url;
        while (*c && *c != ',') c++;
        if (!*c) return;
        int isb64 = 0;
        for (const char *q = p->url; q < c - 5; q++)
            if (strncmp(q, "base64", 6) == 0) { isb64 = 1; break; }
        if (!isb64) return;
        c++;
        int n = 0, bits = 0, acc = 0;
        for (; *c && n < MAX_IMG_BYTES; c++) {
            int v;
            if (*c >= 'A' && *c <= 'Z') v = *c - 'A';
            else if (*c >= 'a' && *c <= 'z') v = *c - 'a' + 26;
            else if (*c >= '0' && *c <= '9') v = *c - '0' + 52;
            else if (*c == '+') v = 62;
            else if (*c == '/') v = 63;
            else continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) { bits -= 8; dlbuf[n++] = (unsigned char)(acc >> bits); }
        }
        if (img_load(dlbuf, (unsigned long)n, &p->im)) p->state = PIC_OK;
    } else {
        int n = fetch_cached(p->url, (char *)dlbuf, MAX_IMG_BYTES);
        if (n > 0 && img_load(dlbuf, (unsigned long)n, &p->im)) p->state = PIC_OK;
        else snprintf(pic_err, sizeof pic_err, "%s",
                      n <= 0 ? "not fetched" : img_err);
    }

    if (p->state == PIC_OK) {
        /* Scale once, here, to whatever it will actually be drawn at: the size
         * the markup asked for, or failing that something the window can hold.
         * A page of photographs cannot then fill memory with pixels nobody
         * will ever see. */
        int maxw = g->w - 2 * MARGIN - 10;
        int maxh = (g->h - VIEW_TOP - STAT_H) * 3;
        if (maxw < 32) maxw = 32;
        if (p->want_w > 0 && p->want_h > 0) {
            if (p->want_w < maxw) maxw = p->want_w;
            if (p->want_h < maxh) maxh = p->want_h;
        }
        if (!pic_fit(&p->im, maxw, maxh)) { img_free(&p->im); p->state = PIC_FAIL; }
    }
}

/* The page shown when nothing else was asked for. Written in the same HTML the
 * parser has to handle, which makes it a small test of it as well. */
/* --- bookmarks -------------------------------------------------------------
 *
 * A file of lines, `url<tab>title`, kept in the OS's own filesystem beside the
 * downloads. The list is shown as a page rather than as a panel, because a
 * bookmark is a link and this browser already knows what to do with those:
 * building the page costs a few lines and gets clicking, scrolling, find and
 * the address bar for nothing.
 */
#define BOOKMARK_FILE "/home/bookmarks.txt"
#define MAX_MARKS     128

typedef struct {
    char url[512];
    char title[160];
} mark_t;

static mark_t marks[MAX_MARKS];
static int    nmarks;
static int    marks_read;        /* the file has been looked at */

static void marks_load(void) {
    if (marks_read) return;
    marks_read = 1;
    nmarks = 0;

    long fd = xyuos_open(BOOKMARK_FILE);
    if (fd < 0) return;                      /* none yet, which is not a fault */

    static char buf[MAX_MARKS * 700];
    long n = xyuos_read(fd, buf, sizeof buf - 1);
    xyuos_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    int i = 0;
    while (i < n && nmarks < MAX_MARKS) {
        int begin = i;
        while (i < n && buf[i] != '\n') i++;
        int end = i;
        if (i < n) i++;
        if (end > begin && buf[end - 1] == '\r') end--;
        if (end <= begin) continue;

        /* url<tab>title, and a line with no tab is a url on its own. */
        int tab = begin;
        while (tab < end && buf[tab] != '\t') tab++;

        mark_t *m = &marks[nmarks];
        int k = 0;
        for (int c = begin; c < tab && k < (int)sizeof m->url - 1; c++)
            m->url[k++] = buf[c];
        m->url[k] = 0;
        k = 0;
        for (int c = (tab < end ? tab + 1 : end); c < end &&
                                                 k < (int)sizeof m->title - 1; c++)
            m->title[k++] = buf[c];
        m->title[k] = 0;
        if (!m->title[0]) snprintf(m->title, sizeof m->title, "%s", m->url);
        if (m->url[0]) nmarks++;
    }
}

static void marks_write(void) {
    static char buf[MAX_MARKS * 700];
    int n = 0;
    for (int i = 0; i < nmarks && n < (int)sizeof buf - 700; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\t%s\n",
                      marks[i].url, marks[i].title);

    xyuos_unlink(BOOKMARK_FILE);
    if (xyuos_create(BOOKMARK_FILE) != 0) {
        snprintf(status, sizeof status, "could not write " BOOKMARK_FILE);
        return;
    }
    long fd = xyuos_open(BOOKMARK_FILE);
    if (fd < 0) {
        snprintf(status, sizeof status, "could not open " BOOKMARK_FILE);
        return;
    }
    int done = 0;
    while (done < n) {
        long w = xyuos_writefd(fd, buf + done, (unsigned long)(n - done));
        if (w <= 0) break;
        done += (int)w;
    }
    xyuos_close(fd);
}

static int mark_of(const char *url) {
    for (int i = 0; i < nmarks; i++)
        if (!strcmp(marks[i].url, url)) return i;
    return -1;
}

/* Keep this page, or stop keeping it. One key for both, because the answer to
 * "is this bookmarked?" and the way to change it are the same question. */
static void mark_toggle(void) {
    marks_load();
    if (!cur_url[0] || !strncmp(cur_url, "about:", 6)) {
        snprintf(status, sizeof status, "there is no page here to keep");
        return;
    }

    int at = mark_of(cur_url);
    if (at >= 0) {
        for (int i = at; i + 1 < nmarks; i++) marks[i] = marks[i + 1];
        nmarks--;
        marks_write();
        snprintf(status, sizeof status, "no longer kept -- %d bookmarks", nmarks);
        return;
    }
    if (nmarks >= MAX_MARKS) {
        snprintf(status, sizeof status, "that is as many bookmarks as there is room for");
        return;
    }
    mark_t *m = &marks[nmarks++];
    snprintf(m->url, sizeof m->url, "%s", cur_url);
    snprintf(m->title, sizeof m->title, "%s",
             page_title[0] ? page_title : cur_url);
    marks_write();
    snprintf(status, sizeof status, "kept -- %d bookmarks", nmarks);
}

/* Text into a page, so a title with an ampersand in it is still a title. */
static int html_escape(const char *in, char *out, int n, int max) {
    for (const char *c = in; *c && n + 8 < max; c++) {
        if (*c == '&')      n += snprintf(out + n, (size_t)(max - n), "&amp;");
        else if (*c == '<') n += snprintf(out + n, (size_t)(max - n), "&lt;");
        else if (*c == '>') n += snprintf(out + n, (size_t)(max - n), "&gt;");
        else if (*c == '"') n += snprintf(out + n, (size_t)(max - n), "&quot;");
        else                out[n++] = *c;
    }
    out[n] = 0;
    return n;
}

static const char *HOME_PAGE =
    "<h1>xyuOS Neo</h1>"
    "<p>A browser. It reads HTML and a useful part of CSS, runs the page's "
    "JavaScript, fills in forms and sends them. Pictures arrive after the "
    "page does.</p>"
    "<h3>Somewhere to start</h3>"
    "<ul>"
    "<li><a href=\"http://example.com/\">example.com</a> - the smallest page on the web</li>"
    "<li><a href=\"https://example.com/\">the same thing over https</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: operating system</a></li>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
    "<li><a href=\"https://www.kernel.org/\">kernel.org</a></li>"
    "</ul>"
    "<h3>Getting around</h3>"
    "<p>Type an address and press <b>Enter</b>. Click a link to follow it. "
    "<b>Backspace</b> goes back, the wheel and the arrow keys scroll, "
    "<b>Home</b> and <b>End</b> jump to the ends.</p>"
    "<h3>Keys</h3>"
    "<ul>"
    "<li><b>t</b> - a new tab; <b>w</b> closes this one</li>"
    "<li><b>1</b>..<b>9</b>, or <b>[</b> and <b>]</b> - which tab</li>"
    "<li><b>l</b> - the address bar</li>"
    "<li><b>/</b> - find on this page; Enter for the next, Esc when done</li>"
    "<li><b>b</b> - keep this page, or stop keeping it</li>"
    "<li><b>m</b> - the pages being kept</li>"
    "<li><b>s</b> - save the link under the pointer, or this page</li>"
    "<li><b>n</b> - what was fetched, and how long it took</li>"
    "<li><b>Tab</b> - the next thing on the page you can type in</li>"
    "</ul>"
    "<p><a href=\"about:bookmarks\">Bookmarks</a></p>"
    "<hr>";

/* The bookmarks, as a page. Built into the html buffer rather than a string
 * of its own, because the tree keeps pointers into whatever it was parsed
 * from and that buffer has to outlive the parse. */
static void show_marks(gui_t *g) {
    marks_load();

    int n = 0;
    n += snprintf(html + n, (size_t)(MAX_HTML - n),
                  "<h1>Bookmarks</h1>");
    if (!nmarks) {
        n += snprintf(html + n, (size_t)(MAX_HTML - n),
                      "<p>Nothing kept yet. Press <b>b</b> on a page to keep "
                      "it, and <b>b</b> again to stop.</p>");
    } else {
        n += snprintf(html + n, (size_t)(MAX_HTML - n), "<ul>");
        for (int i = 0; i < nmarks && n < MAX_HTML - 2048; i++) {
            char u[1024], t[512];
            html_escape(marks[i].url, u, 0, (int)sizeof u);
            html_escape(marks[i].title, t, 0, (int)sizeof t);
            n += snprintf(html + n, (size_t)(MAX_HTML - n),
                          "<li><a href=\"%s\">%s</a><br><small>%s</small></li>",
                          u, t, u);
        }
        n += snprintf(html + n, (size_t)(MAX_HTML - n), "</ul>");
    }
    n += snprintf(html + n, (size_t)(MAX_HTML - n),
                  "<hr><p><a href=\"about:start\">Start page</a></p>");

    snprintf(cur_url, sizeof cur_url, "about:bookmarks");
    css_reset();
    nsheets = 0;
    parse_html(html, n);
    layout(g, g->w - 2 * MARGIN - 10);
    scroll = 0;
    run_scripts();
    snprintf(status, sizeof status, "%d bookmark%s",
             nmarks, nmarks == 1 ? "" : "s");
}

static void show_home(gui_t *g) {
    snprintf(cur_url, sizeof cur_url, "about:start");
    css_reset();
    nsheets = 0;
    parse_html(HOME_PAGE, (int)strlen(HOME_PAGE));
    layout(g, g->w - 2 * MARGIN - 10);
    scroll = 0;
    /* No scripts here, but this clears the last page's -- otherwise its
     * timers keep firing at a document that is no longer on screen. */
    run_scripts();
    snprintf(status, sizeof status, "%d links", nlinks);
}

/* Draw one frame right now, without waiting for the main loop. Used to get a
 * message on screen BEFORE a fetch, since the fetch itself does not return
 * until it is finished. */
static void say(gui_t *g, const char *msg) {
    snprintf(status, sizeof status, "%s", msg);
    if (gui_sync(g)) { draw(g); gui_present(g); }
}

/* Everything a query string may not carry, written as %XX. */
static int url_escape(const char *s, char *out, int n, int max) {
    static const char *hex = "0123456789ABCDEF";
    for (; *s && n + 3 < max; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else if (c == ' ') {
            out[n++] = '+';
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return n;
}

static void navigate(gui_t *g, const char *url, int record);

/* Send a form. `pressed` is the button that was used, which is the only
 * button whose name and value go along -- a form with two of them is asking
 * which one you meant.
 *
 * Only GET is carried out: it is the whole of search, and it is a request
 * this browser already knows how to make. A POST needs a body, and the
 * transport under here sends none, so saying so plainly beats sending the
 * wrong request and showing whatever comes back. */
static void submit_form(gui_t *g, int pressed) {
    if (pressed < 0 || pressed >= nfields) return;
    int form = fields[pressed].form;
    /* A control that is not in a form has nothing to submit. Pressing Enter
     * in one is how a page runs a script, not how it asks for a page. */
    if (form < 0) return;

    char action[1024] = "";
    char method[16] = "";
    if (form >= 0) {
        attr_value(dom[form].attr, dom[form].attrlen, "action", action, sizeof action);
        attr_value(dom[form].attr, dom[form].attrlen, "method", method, sizeof method);
    }
    int posting = ci_eq(method, "post");

    char query[2048];
    int n = 0;
    for (int i = 0; i < nfields; i++) {
        field_t *f = &fields[i];
        if (f->form != form || f->disabled || !f->name[0]) continue;
        if ((f->kind == FLD_CHECK || f->kind == FLD_RADIO) && !f->checked) continue;
        if (f->kind == FLD_BUTTON) continue;
        if (f->kind == FLD_SUBMIT && i != pressed) continue;

        const char *v = f->value;
        if (f->kind == FLD_SELECT)
            v = (f->opt >= 0 && f->opt < f->nopt) ? f->optval[f->opt] : "";
        if (f->kind == FLD_SUBMIT) v = f->label;

        if (n && n + 1 < (int)sizeof query) query[n++] = '&';
        n = url_escape(f->name, query, n, (int)sizeof query - 2);
        if (n + 1 < (int)sizeof query) query[n++] = '=';
        n = url_escape(v, query, n, (int)sizeof query - 1);
    }
    query[n] = 0;

    /* An action of its own, or this page again -- and either way without
     * whatever query string it already carried. */
    char base[1024];
    if (action[0]) url_resolve(cur_url, action, base, sizeof base);
    else           snprintf(base, sizeof base, "%s", cur_url);
    for (char *c = base; *c; c++)
        if (*c == '?' || *c == '#') { *c = 0; break; }

    focus_field = -1;

    if (posting) {
        /* The fields go in the body instead of the address, which is the
         * whole of what POST means to a form. */
        int k = n < (int)sizeof post_body - 1 ? n : (int)sizeof post_body - 1;
        for (int i = 0; i < k; i++) post_body[i] = query[i];
        post_body[k] = 0;
        post_len = k;
        navigate(g, base, 1);
        return;
    }

    char next[1024 + 2048];
    snprintf(next, sizeof next, "%s%s%s", base, n ? "?" : "", query);
    navigate(g, next, 1);
}

/* --- cookies ---------------------------------------------------------------
 *
 * There were none. The browser can post a sign-in form and the server answers
 * with a session -- and the browser threw it away on the spot, so the next
 * request arrived as a stranger and no site could remember anything.
 *
 * The machine has a real clock, so an expiry is a real expiry rather than a
 * guess, and cookies that outlive the session are written to the OS's own
 * filesystem beside the bookmarks.
 */
#define COOKIE_FILE "/home/cookies.txt"
#define MAX_COOKIES 128

typedef struct {
    char host[128];        /* the domain it belongs to */
    char path[128];
    char name[96];
    char value[640];
    unsigned char secure;  /* https only */
    unsigned char host_only;   /* no Domain= was given: this host and no other */
    long long expires;     /* seconds since the epoch, or 0 for this session */
} cookie_t;

static cookie_t cookies[MAX_COOKIES];
static int      ncookies;
static int      cookies_read;

/* A count of seconds does not fit in an int past 2038, and this libc has no
 * atoll, so here is one. */
static long long ll_of(const char *v) {
    long long n = 0;
    int neg = 0;
    while (*v == ' ') v++;
    if (*v == '-') { neg = 1; v++; }
    else if (*v == '+') v++;
    while (*v >= '0' && *v <= '9') {
        if (n > 900000000000000000LL) break;      /* nothing sane gets here */
        n = n * 10 + (*v++ - '0');
    }
    return neg ? -n : n;
}

/* Seconds since 1970, out of the clock on the board. Days-from-civil, which
 * is the usual way of doing it without a table of month lengths. */
static long long now_epoch(void) {
    struct xyuos_tm t;
    xyuos_time(&t);
    int y = t.year, m = t.mon, d = t.day;
    if (y < 1970) return 0;
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
}

/* Does a cookie for `chost` reach `host`? Either it is that host, or the
 * cookie names a domain and the host ends in it on a label boundary -- so
 * that a cookie for example.com reaches www.example.com and one for
 * ample.com does not. */
static int domain_reaches(const cookie_t *c, const char *host) {
    if (!ci_eq(c->host, host)) {
        if (c->host_only) return 0;
        int hl = (int)strlen(host), cl = (int)strlen(c->host);
        if (cl >= hl) return 0;
        if (host[hl - cl - 1] != '.') return 0;
        if (!ci_eq(host + hl - cl, c->host)) return 0;
    }
    return 1;
}

/* A cookie's path reaches a request path when the request is that path or
 * something under it. */
static int path_reaches(const char *cpath, const char *path) {
    int n = (int)strlen(cpath);
    if (n == 0) return 1;
    if (strncmp(path, cpath, (size_t)n) != 0) return 0;
    return path[n] == 0 || path[n] == '/' || cpath[n - 1] == '/';
}

static int cookie_slot(const char *host, const char *path, const char *name) {
    for (int i = 0; i < ncookies; i++)
        if (ci_eq(cookies[i].name, name) && ci_eq(cookies[i].host, host) &&
            !strcmp(cookies[i].path, path))
            return i;
    return -1;
}

static void cookie_drop(int i) {
    for (int k = i; k + 1 < ncookies; k++) cookies[k] = cookies[k + 1];
    ncookies--;
}

static void cookies_write(void) {
    static char buf[MAX_COOKIES * 1100];
    int n = 0;
    long long now = now_epoch();
    for (int i = 0; i < ncookies && n < (int)sizeof buf - 1100; i++) {
        cookie_t *c = &cookies[i];
        if (!c->expires || c->expires <= now) continue;   /* this session only */
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\t%s\t%s\t%s\t%d\t%d\t%lld\n",
                      c->host, c->path, c->name, c->value,
                      c->secure, c->host_only, c->expires);
    }
    xyuos_unlink(COOKIE_FILE);
    if (xyuos_create(COOKIE_FILE) != 0) return;
    long fd = xyuos_open(COOKIE_FILE);
    if (fd < 0) return;
    int done = 0;
    while (done < n) {
        long w = xyuos_writefd(fd, buf + done, (unsigned long)(n - done));
        if (w <= 0) break;
        done += (int)w;
    }
    xyuos_close(fd);
}

static void cookies_load(void) {
    if (cookies_read) return;
    cookies_read = 1;
    long fd = xyuos_open(COOKIE_FILE);
    if (fd < 0) return;
    static char buf[MAX_COOKIES * 1100];
    long n = xyuos_read(fd, buf, sizeof buf - 1);
    xyuos_close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    long long now = now_epoch();
    int i = 0;
    while (i < n && ncookies < MAX_COOKIES) {
        int begin = i;
        while (i < n && buf[i] != '\n') i++;
        int end = i;
        if (i < n) i++;
        if (end <= begin) continue;
        buf[end] = 0;

        /* host, path, name, value, secure, host_only, expires */
        char *f[7];
        int nf = 0;
        char *at = buf + begin;
        f[nf++] = at;
        for (char *c = at; *c && nf < 7; c++)
            if (*c == '\t') { *c = 0; f[nf++] = c + 1; }
        if (nf != 7) continue;

        cookie_t *c = &cookies[ncookies];
        memset(c, 0, sizeof *c);
        snprintf(c->host,  sizeof c->host,  "%s", f[0]);
        snprintf(c->path,  sizeof c->path,  "%s", f[1]);
        snprintf(c->name,  sizeof c->name,  "%s", f[2]);
        snprintf(c->value, sizeof c->value, "%s", f[3]);
        c->secure = (unsigned char)atoi(f[4]);
        c->host_only = (unsigned char)atoi(f[5]);
        c->expires = ll_of(f[6]);
        if (c->expires > now) ncookies++;      /* the rest are already gone */
    }
}

/* The three-letter months, in the order a date header writes them. */
static int month_of(const char *m) {
    static const char *M = "janfebmaraprmayjunjulaugsepoctnovdec";
    char t[4];
    for (int i = 0; i < 3; i++)
        t[i] = (char)((m[i] >= 'A' && m[i] <= 'Z') ? m[i] + 32 : m[i]);
    for (int i = 0; i < 12; i++)
        if (!strncmp(M + i * 3, t, 3)) return i + 1;
    return 0;
}

/* `Wdy, DD Mon YYYY HH:MM:SS GMT`, which is the shape an Expires is written
 * in. Anything else is read as no expiry at all, which makes it a cookie for
 * this session -- the safe way round to be wrong. */
static long long parse_http_date(const char *v) {
    while (*v && *v != ' ') v++;              /* past the day name */
    while (*v == ' ' || *v == ',') v++;
    int d = atoi(v);
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int mo = month_of(v);
    if (!mo || d <= 0) return 0;
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int y = atoi(v);
    while (*v && *v != ' ') v++;
    while (*v == ' ') v++;
    int hh = atoi(v), mm = 0, ss = 0;
    const char *c = v;
    while (*c && *c != ':') c++;
    if (*c == ':') { mm = atoi(c + 1); c++; while (*c && *c != ':') c++; }
    if (*c == ':') ss = atoi(c + 1);
    if (y < 1970) return 0;

    int yy = y - (mo <= 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

/* Take in one Set-Cookie line. `host` and `path` are where the response came
 * from, which is what the defaults are taken from. */
static void cookie_take(const char *line, const char *host, const char *path,
                        int over_tls) {
    cookies_load();

    cookie_t c;
    memset(&c, 0, sizeof c);
    c.host_only = 1;
    snprintf(c.host, sizeof c.host, "%s", host);

    /* The default path is the directory the response came from. */
    {
        int cut = 0;
        for (int i = 0; path[i] && i < (int)sizeof c.path - 1; i++)
            if (path[i] == '/') cut = i;
        snprintf(c.path, sizeof c.path, "%.*s", cut ? cut : 1, cut ? path : "/");
    }

    /* name=value, then ; attribute[=value] ... */
    const char *p2 = line;
    while (*p2 == ' ') p2++;
    int k = 0;
    while (*p2 && *p2 != '=' && *p2 != ';' && k < (int)sizeof c.name - 1)
        c.name[k++] = *p2++;
    c.name[k] = 0;
    while (k > 0 && c.name[k - 1] == ' ') c.name[--k] = 0;
    if (!c.name[0]) return;
    if (*p2 == '=') p2++;
    k = 0;
    while (*p2 && *p2 != ';' && k < (int)sizeof c.value - 1) c.value[k++] = *p2++;
    c.value[k] = 0;
    while (k > 0 && c.value[k - 1] == ' ') c.value[--k] = 0;

    long long now = now_epoch();
    long long max_age = -1;
    int deleted = 0;

    while (*p2 == ';') {
        p2++;
        while (*p2 == ' ') p2++;
        char at[32];
        int a = 0;
        while (*p2 && *p2 != '=' && *p2 != ';' && a < (int)sizeof at - 1)
            at[a++] = (char)((*p2 >= 'A' && *p2 <= 'Z') ? *p2 + 32 : *p2), p2++;
        at[a] = 0;
        const char *val = "";
        char vbuf[160];
        if (*p2 == '=') {
            p2++;
            while (*p2 == ' ') p2++;
            int v = 0;
            while (*p2 && *p2 != ';' && v < (int)sizeof vbuf - 1) vbuf[v++] = *p2++;
            vbuf[v] = 0;
            val = vbuf;
        }

        if (!strcmp(at, "domain") && val[0]) {
            const char *d = val;
            if (*d == '.') d++;
            /* A site may only set a cookie for itself or a domain it is
             * under. Anything else is a site claiming somebody else's name. */
            int hl = (int)strlen(host), dl = (int)strlen(d);
            if (ci_eq(d, host) ||
                (dl < hl && host[hl - dl - 1] == '.' && ci_eq(host + hl - dl, d))) {
                snprintf(c.host, sizeof c.host, "%s", d);
                c.host_only = 0;
            }
        } else if (!strcmp(at, "path") && val[0] == '/') {
            snprintf(c.path, sizeof c.path, "%s", val);
        } else if (!strcmp(at, "secure")) {
            c.secure = 1;
        } else if (!strcmp(at, "max-age")) {
            max_age = ll_of(val);
            if (max_age <= 0) deleted = 1;
            else c.expires = now + max_age;
        } else if (!strcmp(at, "expires") && max_age < 0) {
            long long e = parse_http_date(val);
            if (e) {
                c.expires = e;
                if (e <= now) deleted = 1;
            }
        }
    }

    int at2 = cookie_slot(c.host, c.path, c.name);
    if (deleted) {
        if (at2 >= 0) cookie_drop(at2);
    } else if (at2 >= 0) {
        cookies[at2] = c;
    } else if (ncookies < MAX_COOKIES) {
        cookies[ncookies++] = c;
    }
    cookies_write();
}

/* The Cookie header for a request, or an empty string. */
static void cookie_header(const char *host, const char *path, int over_tls,
                          char *out, int max) {
    cookies_load();
    out[0] = 0;
    long long now = now_epoch();
    int n = 0, any = 0;

    for (int i = 0; i < ncookies; i++) {
        cookie_t *c = &cookies[i];
        if (c->expires && c->expires <= now) { cookie_drop(i--); continue; }
        if (c->secure && !over_tls) continue;
        if (!domain_reaches(c, host)) continue;
        if (!path_reaches(c->path, path)) continue;
        if (n + (int)strlen(c->name) + (int)strlen(c->value) + 8 > max) break;
        n += snprintf(out + n, (size_t)(max - n), "%s%s=%s",
                      any ? "; " : "Cookie: ", c->name, c->value);
        any = 1;
    }
    if (any) snprintf(out + n, (size_t)(max - n), "\r\n");
    else out[0] = 0;
}

/* --- what a page is made of, kept ------------------------------------------
 *
 * A page's stylesheets were fetched again every single time the page was,
 * which on github.com is two dozen requests and three megabytes for a click
 * that goes back where it came from. Keeping them costs a few megabytes of
 * memory and is the whole difference between a second visit and a first one.
 *
 * The page itself is never kept. Asking for a page is asking for what is
 * there now; this is only for the parts a page is built out of, which carry
 * a fingerprint in their name and change by changing that name.
 */
#define CACHE_BYTES     (6 * 1024 * 1024)
#define CACHE_SLOTS     128
#define CACHE_MAX_ENTRY (768 * 1024)

typedef struct {
    char url[512];
    int  off, len;
} cache_ent;

static char      *cache_arena;
static int        cache_fill;
static cache_ent  cache_ents[CACHE_SLOTS];
static int        cache_n;
static int        cache_hits, cache_misses, cache_saved;

static int cache_find(const char *url) {
    for (int i = 0; i < cache_n; i++)
        if (!strcmp(cache_ents[i].url, url)) return i;
    return -1;
}

/* Space is handed out by bumping a pointer, so there is nothing to free and
 * nothing to fragment. When it runs out the whole thing is dropped rather
 * than picking victims: six megabytes holds a large site's stylesheets whole,
 * and a cache that is right up against its limit is not helping anybody. */
static void cache_put(const char *url, const char *data, int n) {
    if (!cache_arena || n <= 0 || n > CACHE_MAX_ENTRY) return;
    if ((int)strlen(url) >= (int)sizeof cache_ents[0].url) return;

    if (cache_fill + n > CACHE_BYTES || cache_n >= CACHE_SLOTS) {
        cache_fill = 0;
        cache_n = 0;
    }
    cache_ent *e = &cache_ents[cache_n++];
    snprintf(e->url, sizeof e->url, "%s", url);
    e->off = cache_fill;
    e->len = n;
    memcpy(cache_arena + cache_fill, data, (size_t)n);
    cache_fill += n;
}

/* Fetch, unless it is already in hand. */
static int fetch_cached(const char *url, char *buf, int max) {
    int at = cache_find(url);
    if (at >= 0) {
        int n = cache_ents[at].len;
        if (n > max) n = max;
        memcpy(buf, cache_arena + cache_ents[at].off, (size_t)n);
        if (n < max) buf[n] = 0;
        cache_hits++;
        cache_saved += n;
        return n;
    }
    cache_misses++;
    int n = fetch_url(url, buf, max, 0, 0);
    if (n > 0) cache_put(url, buf, n);
    return n;
}

/* --- saving what was fetched -----------------------------------------------
 *
 * Files land in the OS's own filesystem -- the image this system boots with --
 * and nowhere near the machine's real disk.
 */
#define DOWNLOAD_DIR "/home/downloads"

/* A name a filesystem will take, out of whatever the server offered. */
static void safe_name(const char *in, char *out, int max) {
    int n = 0;
    for (const char *c = in; *c && n < max - 1; c++) {
        unsigned char x = (unsigned char)*c;
        if ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') ||
            (x >= '0' && x <= '9') || x == '.' || x == '-' || x == '_')
            out[n++] = (char)x;
        else if (n && out[n-1] != '_')
            out[n++] = '_';
    }
    while (n && out[n-1] == '_') n--;          /* no trailing filler */
    out[n] = 0;
    if (!out[0]) snprintf(out, (size_t)max, "download");
}

/* What to call it: what the server asked for, else the last piece of the
 * address, else something rather than nothing. */
static void download_name(const char *url, char *out, int max) {
    char raw[256];
    raw[0] = 0;

    /* Content-Disposition: attachment; filename="report.pdf" */
    const char *f = 0;
    for (const char *c = last_disp; *c; c++) {
        if ((c[0] == 'f' || c[0] == 'F') && !strncmp(c + 1, "ilename", 7)) {
            const char *e = c + 8;
            if (*e == '*') e++;
            while (*e == ' ' || *e == '=') e++;
            if (*e == '"' || *e == '\'') e++;
            f = e;
            break;
        }
    }
    if (f) {
        int n = 0;
        while (*f && *f != '"' && *f != '\'' && *f != ';' && n < (int)sizeof raw - 1)
            raw[n++] = *f++;
        raw[n] = 0;
    }

    if (!raw[0]) {
        /* The last piece of the path, without any query string. */
        const char *slash = url, *c = url;
        int inpath = 0;
        for (; *c; c++) {
            if (*c == '?' || *c == '#') break;
            if (*c == '/') {
                if (!inpath && c > url && c[-1] == '/') { inpath = 1; continue; }
                slash = c + 1;
            }
        }
        int n = 0;
        while (slash < c && n < (int)sizeof raw - 1) raw[n++] = *slash++;
        raw[n] = 0;
    }

    safe_name(raw, out, max);
}

/* Write the bytes out, and say where they went. Returns 1 if they got there. */
static int save_bytes(const char *name, const char *data, int n) {
    xyuos_mkdir(DOWNLOAD_DIR);          /* harmless if it is already there */

    char path[320];
    snprintf(path, sizeof path, "%s/%s", DOWNLOAD_DIR, name);

    xyuos_unlink(path);                 /* a second copy replaces the first */
    if (xyuos_create(path) != 0) {
        snprintf(status, sizeof status, "could not make %s", path);
        return 0;
    }
    long fd = xyuos_open(path);
    if (fd < 0) {
        snprintf(status, sizeof status, "could not open %s to write", path);
        return 0;
    }

    int done = 0;
    while (done < n) {
        long w = xyuos_writefd(fd, data + done, (unsigned long)(n - done));
        if (w <= 0) break;
        done += (int)w;
    }
    xyuos_close(fd);

    if (done != n) {
        snprintf(status, sizeof status, "%s: wrote %d of %d bytes", path, done, n);
        return 0;
    }
    snprintf(status, sizeof status, "saved %s -- %d bytes", path, n);
    return 1;
}

/* Is this something to read, or something to keep? */
static int is_page_type(const char *ctype) {
    if (!ctype[0]) return 1;            /* said nothing: try to read it */
    char t[64];
    int n = 0;
    for (const char *c = ctype; *c && *c != ';' && n < (int)sizeof t - 1; c++)
        t[n++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    t[n] = 0;
    return !strcmp(t, "text/html") || !strcmp(t, "application/xhtml+xml") ||
           !strcmp(t, "text/plain") || !strcmp(t, "application/xml") ||
           !strcmp(t, "text/xml");
}

/* Fetch something and keep it, without trying to read it as a page. */
static void download_url(gui_t *g, const char *url) {
    char msg[256];
    snprintf(msg, sizeof msg, "fetching %s ...", url);
    say(g, msg);

    char final[1024];
    int n = fetch_url(url, (char *)dlbuf, MAX_IMG_BYTES, final, sizeof final);
    if (n < 0) return;                  /* fetch_url said why */

    char name[128];
    download_name(final, name, sizeof name);
    save_bytes(name, (const char *)dlbuf, n);
}

static void navigate(gui_t *g, const char *url, int record) {
    if (record && cur_url[0] && nhist < HIST_MAX) {
        snprintf(hist[nhist++], sizeof hist[0], "%s", cur_url);
        nfwd = 0;         /* a new destination ends the road forward */
    }

    if (strncmp(url, "about:", 6) == 0) {
        if (!strcmp(url + 6, "bookmarks")) show_marks(g);
        else                               show_home(g);
        return;
    }

    {   /* The address, without the scheme, is the useful half to show. */
        const char *h = url;
        if (strncmp(h, "http://", 7) == 0) h += 7;
        else if (strncmp(h, "https://", 8) == 0) h += 8;
        char msg[256];
        snprintf(msg, sizeof msg, "loading %s ...", h);
        say(g, msg);
    }

    char final[1024];
    int n = fetch_url(url, html, MAX_HTML, final, sizeof final);
    if (n < 0) {
        /* Keep the page that is already there; the status line says why. */
        return;
    }
    /* Something the browser cannot read is not an error and not a blank
     * page: it is a file, and the thing to do with a file is keep it. */
    if (!is_page_type(last_ctype)) {
        char name[128];
        download_name(final, name, sizeof name);
        save_bytes(name, html, n);
        /* The page that was already on screen stays; only the address bar
         * would have lied about where we are. */
        if (record && nhist > 0) nhist--;
        return;
    }

    snprintf(cur_url, sizeof cur_url, "%s", final);

    /* Kept so that coming back to this tab does not mean asking the server
     * for the page all over again. */
    {
        tab_t *t = &tabs[cur_tab];
        if (!t->src) t->src = (char *)malloc(MAX_HTML);
        if (t->src) {
            int keep = n < MAX_HTML ? n : MAX_HTML;
            memcpy(t->src, html, (size_t)keep);
            t->srclen = keep;
        }
    }

    css_reset();
    nsheets = 0;
    parse_html(html, n);
    layout(g, g->w - 2 * MARGIN - 10);
    scroll = 0;

    /* The page is on screen before its scripts run. They may fetch, and
     * fetching redraws -- so there has to be something finished to redraw. */
    run_scripts();
    if (dom_dirty) {
        dom_dirty = 0;
        render_dom();
        layout(g, g->w - 2 * MARGIN - 10);
    }

    snprintf(status, sizeof status, "%d links%s", nlinks,
             npics ? ", pictures to come" : "");
}

/* Back and forward are the same move in opposite directions: the page being
 * left goes onto the other stack. */
/* A script changed something: build the drawing list again and re-measure.
 * Cheap enough to do outright -- the whole point of keeping the tree. */
/* Put the match being looked at in the middle of the window, unless it is
 * already on screen -- jumping the page around for a match you can already
 * see is worse than not moving at all. */
static void find_show(gui_t *g) {
    if (!find_nmatch) return;
    int si = find_span_of(find_cur);
    if (si < 0) return;
    int it = find_spans[si].item;
    if (it < 0 || it >= nitems) return;

    int page = g->h - VIEW_TOP - STAT_H - MARGIN;
    int y = items[it].y;
    if (y >= scroll && y < scroll + page - g->fh) return;
    scroll = y - page / 2;
    if (scroll > doc_height - page) scroll = doc_height - page;
    if (scroll < 0) scroll = 0;
}

static void reflow(gui_t *g) {
    if (!dom_dirty) return;
    dom_dirty = 0;
    render_dom();
    layout(g, g->w - 2 * MARGIN - 10);
}

#define HIST_BYTES ((size_t)HIST_MAX * 1024)

/* Write down what the tab on screen has become. */
static void tab_save(void) {
    tab_t *t = &tabs[cur_tab];
    snprintf(t->url, sizeof t->url, "%s", cur_url);
    snprintf(t->title, sizeof t->title, "%s", page_title);
    t->scroll = scroll;

    if (!t->hist) t->hist = (char (*)[1024])malloc(HIST_BYTES);
    if (!t->fwd)  t->fwd  = (char (*)[1024])malloc(HIST_BYTES);
    t->nhist = t->nfwd = 0;
    if (t->hist) { memcpy(t->hist, hist, HIST_BYTES); t->nhist = nhist; }
    if (t->fwd)  { memcpy(t->fwd,  fwd,  HIST_BYTES); t->nfwd  = nfwd;  }

    t->nfld = 0;
    for (int i = 0; i < nfields && t->nfld < TAB_FIELDS; i++) {
        field_t *f = &fields[i];
        if (f->kind == FLD_SUBMIT || f->kind == FLD_BUTTON) continue;
        tabfield_t *k = &t->fld[t->nfld++];
        k->node = f->node;
        k->kind = f->kind;
        k->checked = f->checked;
        k->opt = f->opt;
        snprintf(k->value, sizeof k->value, "%s", f->value);
    }
}

/* Put whatever tab is current on the screen. Nothing is saved here: the
 * caller decides whether there was anything worth saving, which is what
 * closing a tab and switching to one differ about. */
static void tab_enter(gui_t *g) {
    tab_t *t = &tabs[cur_tab];

    nhist = nfwd = 0;
    if (t->hist) { memcpy(hist, t->hist, HIST_BYTES); nhist = t->nhist; }
    if (t->fwd)  { memcpy(fwd,  t->fwd,  HIST_BYTES); nfwd  = t->nfwd;  }

    /* None of this belongs to the tab being arrived at. */
    focus_field = -1;
    find_open = 0;
    find_query[0] = 0;

    if (!t->url[0]) { show_home(g); return; }

    /* A page that was built here rather than fetched is built again; anything
     * else is read back out of the bytes the tab kept. */
    if (!strncmp(t->url, "about:", 6) || !t->src || t->srclen <= 0) {
        navigate(g, t->url, 0);
        return;
    }

    memcpy(html, t->src, (size_t)t->srclen);
    if (t->srclen < MAX_HTML) html[t->srclen] = 0;
    snprintf(cur_url, sizeof cur_url, "%s", t->url);
    css_reset();
    nsheets = 0;

    /* Nothing from the tab being left may come across. Rebuilding a page
     * carries values over by element number so that a script changing the
     * page does not empty the search box -- but two different pages parsed
     * into the same shape have the same numbers, and without this the tab
     * being arrived at would be filled in with the other one's answers. */
    nfields = 0;

    parse_html(html, t->srclen);
    layout(g, g->w - 2 * MARGIN - 10);
    scroll = t->scroll < 0 ? 0 : t->scroll;

    /* Its own answers, before its own scripts get to read them. */
    for (int i = 0; i < nfields; i++) {
        for (int k = 0; k < t->nfld; k++) {
            if (t->fld[k].node != fields[i].node) continue;
            if (t->fld[k].kind != fields[i].kind) break;
            snprintf(fields[i].value, sizeof fields[i].value, "%s", t->fld[k].value);
            fields[i].checked = t->fld[k].checked;
            fields[i].opt = t->fld[k].opt;
            fields[i].cur = (int)strlen(fields[i].value);
            break;
        }
    }

    run_scripts();
    if (dom_dirty) {
        dom_dirty = 0;
        render_dom();
        layout(g, g->w - 2 * MARGIN - 10);
    }
    snprintf(status, sizeof status, "tab %d of %d", cur_tab + 1, ntabs);
}

static void tab_show(gui_t *g, int i) {
    if (i < 0 || i >= ntabs || i == cur_tab) return;
    tab_save();
    cur_tab = i;
    tab_enter(g);
}

static void tab_new(gui_t *g) {
    if (ntabs >= MAX_TABS) {
        snprintf(status, sizeof status, "that is as many tabs as there is room for");
        return;
    }
    tab_save();
    cur_tab = ntabs++;
    tab_t *t = &tabs[cur_tab];
    t->url[0] = t->title[0] = 0;
    t->srclen = 0;
    t->scroll = 0;
    t->nhist = t->nfwd = 0;
    tab_enter(g);                  /* no url: the start page */
    snprintf(status, sizeof status, "tab %d of %d", cur_tab + 1, ntabs);
}

static void tab_close(gui_t *g) {
    if (ntabs <= 1) {
        snprintf(status, sizeof status, "that is the only tab");
        return;
    }
    tab_t *going = &tabs[cur_tab];
    free(going->src);  going->src  = 0;
    free(going->hist); going->hist = 0;
    free(going->fwd);  going->fwd  = 0;

    for (int i = cur_tab; i + 1 < ntabs; i++) tabs[i] = tabs[i + 1];
    ntabs--;
    memset(&tabs[ntabs], 0, sizeof tabs[ntabs]);

    /* The one on its left, or the first if it was the first. Nothing is
     * saved: the tab that would have been saved into is the one just gone. */
    if (cur_tab > 0) cur_tab--;
    if (cur_tab >= ntabs) cur_tab = ntabs - 1;
    tab_enter(g);
    snprintf(status, sizeof status, "tab %d of %d", cur_tab + 1, ntabs);
}

static void go_back(gui_t *g) {
    if (nhist <= 0) return;
    if (nfwd < HIST_MAX) snprintf(fwd[nfwd++], sizeof fwd[0], "%s", cur_url);
    char to[1024];
    snprintf(to, sizeof to, "%s", hist[--nhist]);
    navigate(g, to, 0);
}

static void go_fwd(gui_t *g) {
    if (nfwd <= 0) return;
    if (nhist < HIST_MAX) snprintf(hist[nhist++], sizeof hist[0], "%s", cur_url);
    char to[1024];
    snprintf(to, sizeof to, "%s", fwd[--nfwd]);
    navigate(g, to, 0);
}

/* --- the window ------------------------------------------------------------ */

static char urlbuf[1024];
static gui_edit_t urledit = { urlbuf, sizeof urlbuf, 0, 0 };
static int url_focus;

#define NBTN 4
static const char *BTN_LABEL[NBTN] = { "Back", "Fwd", "Reload", "Net" };
static const int   BTN_W[NBTN]     = {   52,    46,      62,      44 };

/* Button i, or i == NBTN for the address box that follows them. */
static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    (void)g;
    int bx = 8;
    for (int k = 0; k < i && k < NBTN; k++) bx += BTN_W[k] + 5;
    *w = (i < NBTN) ? BTN_W[i] : 60;
    *h = 24;
    *x = bx;
    *y = (TOOL_H - *h) / 2;
}

/* --- the network panel -----------------------------------------------------
 * What the kernel recorded for each fetch. This replaces the running commentary
 * that used to be printed straight onto the screen from inside the TLS code --
 * the same information, in a place the user opens on purpose. */
static void draw_netlog(gui_t *g) {
    static struct net_log_ent ents[NET_ROWS];
    int n = net_log(ents, NET_ROWS);

    int x = MARGIN, y = TOOL_H + 6;
    int w = g->w - 2 * MARGIN, h = g->h - VIEW_TOP - STAT_H - 12;
    if (w < 80 || h < 40) return;
    gui_panel(g, x, y, w, h, GC_PANEL, GC_EDGE);

    int rh = g->fh + 4;
    int tx = x + 8, ty = y + 6;

    /* A header row, then a rule under it. */
    gui_text(g, tx, ty, "   ms    bytes  code  conn   address", GC_DIM);
    gui_fill(g, x + 4, ty + rh - 2, w - 8, 1, GC_LINE);
    ty += rh + 2;

    /* Three areas, measured before any of them is drawn: the requests, the
     * console beneath them, and the summary on the last row. Working each one
     * out against the whole panel is what had the console drawing straight
     * over the list. */
    int con_show = console_n < 8 ? console_n : 8;
    int con_h    = con_show ? rh * (con_show + 1) + 6 : 0;
    int sum_y    = y + h - rh;
    int con_y    = sum_y - 6 - con_h;

    int rows = (con_y - ty) / rh;
    if (rows < 1) rows = 1;
    int first = n - rows - net_scroll;
    if (first < 0) first = 0;

    int shown = 0, handshakes = 0, reused = 0, total_ms = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i].flags & NET_LOG_REUSED) reused++;
        else if (ents[i].flags & NET_LOG_TLS) handshakes++;
        total_ms += (int)ents[i].dur_ms;
    }

    for (int i = first; i < n && shown < rows; i++, shown++) {
        struct net_log_ent *e = &ents[i];
        int ry = ty + shown * rh;
        if (shown & 1) gui_fill(g, x + 2, ry - 2, w - 4, rh, GC_ALT);

        char line[320];
        char code[8];
        if (e->status) snprintf(code, sizeof code, "%d", e->status);
        else           snprintf(code, sizeof code, "--");

        /* "kept" is the whole point of the column: no handshake was needed. */
        const char *conn = (e->flags & NET_LOG_CACHED) ? "cache"
                         : (e->flags & NET_LOG_REUSED) ? "kept "
                         : (e->flags & NET_LOG_TLS)    ? "tls  " : "new  ";

        snprintf(line, sizeof line, "%5u  %7d  %4s  %s  %s%s",
                 e->dur_ms, e->bytes, code, conn, e->host, e->path);

        unsigned c = (e->flags & NET_LOG_FAIL) ? GC_WARN
                   : (e->flags & NET_LOG_REUSED) ? GC_GOOD : GC_TEXT;
        gui_text_clip(g, tx, ry, line, c, w - 16);
    }

    if (n == 0)
        gui_text(g, tx, ty, "nothing fetched yet", GC_DIM);

    /* --- what the page's own code said ---------------------------------- */
    if (con_show && con_y > ty) {
        gui_fill(g, x + 4, con_y - 4, w - 8, 1, GC_LINE);
        gui_text(g, tx, con_y, "console", GC_DIM);
        for (int i = 0; i < con_show; i++) {
            int idx = console_n - con_show + i;
            gui_text_clip(g, tx, con_y + rh * (i + 1),
                          console_log[idx % CONSOLE_LINES], GC_TEXT, w - 16);
        }
    }

    /* The summary is the answer to "why was that slow", in one line. */
    char sum[256];
    snprintf(sum, sizeof sum,
             "%d req %d tls %d kept %dms | cache %d/%d %d KB | %d el %d rules "
             "%d vars %d sheets | css %d+%d draw %d ms",
             n, handshakes, reused, total_ms,
             cache_hits, cache_hits + cache_misses, cache_saved / 1024,
             page_elems, css_nrules,
             css_var_n, nsheets,
             page_css_fetch_ms, page_style_ms, page_render_ms);
    gui_fill(g, x + 4, sum_y - 4, w - 8, 1, GC_LINE);
    gui_text_clip(g, tx, sum_y, sum, GC_DIM, w - 16);
}

/* Where a tab's button is. The strip is only drawn when there is more than
 * one, so with a single tab this is never asked. */
static void tab_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int each = g->w / (ntabs > 0 ? ntabs : 1);
    if (each > 260) each = 260;
    *x = i * each;
    *y = TOOL_H;
    *w = each - 1;
    *h = TAB_H - 1;
}

static void draw(gui_t *g) {
    gui_clear(g, GC_WIN);

    /* the page */
    if (ntabs > 1) {
        gui_vgrad(g, 0, TOOL_H, g->w, TAB_H, GC_BAR, GC_BAR2);
        for (int i = 0; i < ntabs; i++) {
            int x, y, w, h;
            tab_rect(g, i, &x, &y, &w, &h);
            int on = (i == cur_tab);
            gui_fill(g, x, y, w, h, on ? GC_WIN : GC_PANEL);
            if (on) gui_fill(g, x, y, w, 2, GC_ACCENT);
            gui_fill(g, x + w, y, 1, h, GC_EDGE);

            /* Its own name, or its address, or that it has neither yet. */
            const char *lab = (i == cur_tab)
                            ? (page_title[0] ? page_title
                               : (cur_url[0] ? cur_url : "new tab"))
                            : (tabs[i].title[0] ? tabs[i].title
                               : (tabs[i].url[0] ? tabs[i].url : "new tab"));
            gui_text_clip(g, x + 6, y + (h - g->fh) / 2, lab,
                          on ? GC_TEXT : GC_DIM, w - 12);
        }
        gui_fill(g, 0, TOOL_H + TAB_H - 1, g->w, 1, GC_EDGE);
    }

    gui_fill(g, 0, VIEW_TOP, g->w, g->h - VIEW_TOP - STAT_H, GC_WIN);
    if (show_net) {
        draw_netlog(g);
    } else {
        draw_page(g);
        int vh = g->h - VIEW_TOP - STAT_H - MARGIN;
        if (doc_height > vh)
            gui_scrollbar(g, g->w - 10, TOOL_H + 2, 8, vh, scroll, vh, doc_height);
    }

    /* The toolbar goes on last, over the page rather than under it. A line
     * of text that begins above the top of the window is still drawn -- half
     * a line of reading is better than none -- and without this it is drawn
     * across the address bar. The status line already worked this way. */
    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);

    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int on = (i == 0) ? (nhist > 0) : (i == 1) ? (nfwd > 0) : 1;
        int st = !on ? GB_OFF
               : (i == 3 && show_net) ? GB_DOWN
               : gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        gui_button(g, x, y, w, h, BTN_LABEL[i], st);
    }
    {
        int x, y, w, h;
        btn_rect(g, NBTN, &x, &y, &w, &h);
        int ex = x, ey = y, ew = g->w - ex - 10, eh = h;
        gui_edit_draw(g, &urledit, ex, ey, ew, eh, url_focus,
                      (uptime_ms() / 500) & 1, "address");
    }


    /* status */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    {
        const char *msg = status;
        char hov[600];
        char fnd[160];
        if (find_open) {
            /* The bar takes the status line while it is up: it is the thing
             * being used, and two lines for one browser is one too many. */
            if (!find_query[0])
                snprintf(fnd, sizeof fnd, "find: _");
            else if (find_nmatch)
                snprintf(fnd, sizeof fnd, "find: %s_    %d of %d   "
                                          "(enter next, shift-enter back, esc done)",
                         find_query, find_cur + 1, find_nmatch);
            else
                snprintf(fnd, sizeof fnd, "find: %s_    not on this page", find_query);
            msg = fnd;
        } else if (hover_link >= 0) {
            snprintf(hov, sizeof hov, "-> %s", hrefbuf + linkoff[hover_link]);
            msg = hov;
        }
        gui_text_clip(g, 10, g->h - STAT_H + (STAT_H - g->fh) / 2, msg,
                      GC_TEXT, g->w - 20);
    }
}

int main(int argc, char **argv) {
    gui_t g;
    if (!gui_open(&g)) return 1;
    G = &g;

    html    = malloc(MAX_HTML);
    textbuf = malloc((size_t)MAX_TEXT * sizeof(unsigned short));
    items   = malloc((size_t)MAX_ITEMS * sizeof(item_t));
    hrefbuf = malloc(MAX_HREF);
    linkoff = malloc((size_t)MAX_LINKS * sizeof(int));
    dlbuf   = malloc(MAX_IMG_BYTES);
    cache_arena = malloc(CACHE_BYTES);   /* a cache is a luxury: if there is
                                          * no room for one, go without */
    css_init();
    node_style = (css_style_t *)malloc((size_t)DOM_MAX_NODES * sizeof(css_style_t));
    node_i0 = (int *)malloc((size_t)DOM_MAX_NODES * sizeof(int));
    node_i1 = (int *)malloc((size_t)DOM_MAX_NODES * sizeof(int));
    int have_dom = dom_init() && js_init() &&
                   node_style && node_i0 && node_i1;
    if (!html || !textbuf || !items || !hrefbuf || !linkoff || !dlbuf || !have_dom) {
        printf("web: not enough memory\n");
        gui_close(&g);
        return 1;
    }

    if (argc > 1) {
        char u[1024];
        if (strncmp(argv[1], "http", 4) == 0) snprintf(u, sizeof u, "%s", argv[1]);
        else snprintf(u, sizeof u, "https://%s", argv[1]);
        show_home(&g);
        navigate(&g, u, 1);
    } else {
        show_home(&g);
    }
    gui_edit_set(&urledit, cur_url);

    int running = 1, dirty = 1;
    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                key_event_t *k = &e.k;
                dirty = 1;
                if (k->code == XKEY_RESIZE) {
                    gui_sync(&g);
                    layout(&g, g.w - 2 * MARGIN - 10);
                    continue;
                }

                /* The find bar has the keyboard while it is up. */
                if (find_open) {
                    int fl = (int)strlen(find_query);
                    if (k->code == XKEY_ESC) {
                        find_open = 0;
                        find_query[0] = 0;
                        find_scan();
                        dirty = 1;
                        continue;
                    }
                    if (k->code == XKEY_ENTER) {
                        if (find_nmatch) {
                            find_cur += (k->mods & XMOD_SHIFT) ? -1 : 1;
                            if (find_cur >= find_nmatch) find_cur = 0;
                            if (find_cur < 0) find_cur = find_nmatch - 1;
                            find_show(&g);
                        }
                        dirty = 1;
                        continue;
                    }
                    if (k->code == XKEY_BKSP) {
                        if (fl) find_query[fl - 1] = 0;
                        find_cur = 0;
                        find_scan();
                        find_show(&g);
                        dirty = 1;
                        continue;
                    }
                    if (k->code == XKEY_CHAR && k->ascii >= 32 && k->ascii < 127 &&
                        fl + 1 < FIND_MAX_Q) {
                        int c = k->ascii;
                        find_query[fl] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
                        find_query[fl + 1] = 0;
                        find_cur = 0;
                        find_scan();
                        find_show(&g);
                        dirty = 1;
                        continue;
                    }
                    /* Arrows and page keys still scroll, so you can look
                     * around without closing the bar. */
                }

                /* A focused field takes the keys, ahead of the page's own
                 * shortcuts -- otherwise typing "n" in a search box opens the
                 * network panel. */
                if (focus_field >= 0 && focus_field < nfields && !url_focus) {
                    field_t *f = &fields[focus_field];
                    int len = (int)strlen(f->value);
                    if (f->cur > len) f->cur = len;

                    if (k->code == XKEY_ESC) { focus_field = -1; dirty = 1; continue; }
                    if (k->code == XKEY_CHAR && k->ascii == 9) {
                        field_step((k->mods & XMOD_SHIFT) ? -1 : 1);
                        dirty = 1;
                        continue;
                    }
                    /* A tick, a button or a list of choices has no text in
                     * it. Everything below this line edits text, so for those
                     * the key belongs to the page. Space is the exception:
                     * that is how a tick is ticked without the mouse. */
                    int typable = field_typable(f);
                    if (!typable) {
                        if (k->code == XKEY_CHAR && k->ascii == ' ') {
                            int nd = f->node;
                            if (f->kind == FLD_CHECK) f->checked = !f->checked;
                            else if (f->kind == FLD_RADIO) {
                                for (int q = 0; q < nfields; q++)
                                    if (fields[q].kind == FLD_RADIO &&
                                        fields[q].form == f->form &&
                                        ci_eq(fields[q].name, f->name))
                                        fields[q].checked = 0;
                                f->checked = 1;
                            } else if (f->kind == FLD_SELECT) {
                                if (f->nopt > 0) f->opt = (f->opt + 1) % f->nopt;
                            } else if (f->kind == FLD_SUBMIT) {
                                jsd_dispatch(nd, "click");
                                reflow(&g);
                                int again = field_of_node(nd);
                                if (again >= 0) submit_form(&g, again);
                                gui_edit_set(&urledit, cur_url);
                                dirty = 1;
                                continue;
                            } else if (f->kind == FLD_BUTTON) {
                                int handled = jsd_dispatch(nd, "click");
                                reflow(&g);
                                if (!handled) {
                                    int lk = link_around(nd);
                                    if (lk >= 0) {
                                        char next[1024];
                                        url_resolve(cur_url,
                                                    hrefbuf + linkoff[lk],
                                                    next, sizeof next);
                                        focus_field = -1;
                                        navigate(&g, next, 1);
                                        gui_edit_set(&urledit, cur_url);
                                    }
                                }
                                dirty = 1;
                                continue;
                            }
                            jsd_dispatch(nd, "change");
                            reflow(&g);
                            dirty = 1;
                            continue;
                        }
                        if (k->code == XKEY_ENTER && f->kind == FLD_SUBMIT) {
                            int nd = f->node;
                            jsd_dispatch(nd, "click");
                            reflow(&g);
                            int again = field_of_node(nd);
                            if (again >= 0) submit_form(&g, again);
                            gui_edit_set(&urledit, cur_url);
                            dirty = 1;
                            continue;
                        }
                        /* Anything else is the browser's, not the field's. */
                        goto not_a_field_key;
                    }

                    if (k->code == XKEY_LEFT)  { if (f->cur > 0) f->cur--; dirty = 1; continue; }
                    if (k->code == XKEY_RIGHT) { if (f->cur < len) f->cur++; dirty = 1; continue; }
                    if (k->code == XKEY_HOME)  { f->cur = 0; dirty = 1; continue; }
                    if (k->code == XKEY_END)   { f->cur = len; dirty = 1; continue; }

                    if (k->code == XKEY_BKSP) {
                        int nd = f->node;
                        if (f->cur > 0) {
                            memmove(f->value + f->cur - 1, f->value + f->cur,
                                    (size_t)(len - f->cur + 1));
                            f->cur--;
                        }
                        dirty = 1;
                        /* Nothing of `f` is touched past here: the page's own
                         * handler can rebuild every field on the page. */
                        jsd_dispatch(nd, "input");
                        reflow(&g);
                        continue;
                    }
                    if (k->code == XKEY_ENTER) {
                        if (f->kind == FLD_AREA) {
                            if (len + 1 < FIELD_VAL) {
                                memmove(f->value + f->cur + 1, f->value + f->cur,
                                        (size_t)(len - f->cur + 1));
                                f->value[f->cur++] = '\n';
                            }
                        } else {
                            /* Enter in a text box is the form's own submit. */
                            int btn = focus_field;
                            for (int i = 0; i < nfields; i++)
                                if (fields[i].kind == FLD_SUBMIT &&
                                    fields[i].form == f->form) { btn = i; break; }
                            submit_form(&g, btn);
                            gui_edit_set(&urledit, cur_url);
                        }
                        dirty = 1;
                        continue;
                    }
                    if (k->code == XKEY_CHAR && k->ascii >= 32 && k->ascii < 127 &&
                        len + 1 < FIELD_VAL) {
                        int nd = f->node;
                        memmove(f->value + f->cur + 1, f->value + f->cur,
                                (size_t)(len - f->cur + 1));
                        f->value[f->cur++] = (char)k->ascii;
                        dirty = 1;
                        jsd_dispatch(nd, "input");
                        reflow(&g);
                        continue;
                    }
                    /* Anything else -- a page key, a scroll -- falls through
                     * to the page, with the field still focused. */
                }
                not_a_field_key:;

                if (!url_focus && focus_field < 0 &&
                    k->code == XKEY_CHAR && k->ascii == 9) {
                    field_step((k->mods & XMOD_SHIFT) ? -1 : 1);
                    dirty = 1;
                    continue;
                }

                if (url_focus) {
                    if (k->code == XKEY_ESC) { url_focus = 0; gui_edit_set(&urledit, cur_url); continue; }
                    if (k->code == XKEY_ENTER) {
                        url_focus = 0;
                        char u[1024];
                        if (strncmp(urlbuf, "http", 4) == 0 || strncmp(urlbuf, "about:", 6) == 0)
                            snprintf(u, sizeof u, "%s", urlbuf);
                        else
                            snprintf(u, sizeof u, "https://%s", urlbuf);
                        navigate(&g, u, 1);
                        gui_edit_set(&urledit, cur_url);
                        continue;
                    }
                    gui_edit_key(&urledit, k);
                    continue;
                }

                int page = g.h - VIEW_TOP - STAT_H - MARGIN;
                switch (k->code) {
                case XKEY_ESC:
                    /* The panel is what Escape closes when it is open; only
                     * then does it close the browser. */
                    if (show_net) { show_net = 0; net_scroll = 0; }
                    else running = 0;
                    break;
                case XKEY_DOWN: scroll += g.fh * 3; break;
                case XKEY_UP:   scroll -= g.fh * 3; break;
                case XKEY_PGDN: scroll += page - g.fh; break;
                case XKEY_PGUP: scroll -= page - g.fh; break;
                case XKEY_HOME: scroll = 0; break;
                case XKEY_END:  scroll = doc_height - page; break;
                case XKEY_BKSP:
                    go_back(&g);
                    gui_edit_set(&urledit, cur_url);
                    break;
                default:
                    if (k->ascii == 't' || k->ascii == 'T') {
                        tab_new(&g);
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii == 'w' || k->ascii == 'W') {
                        tab_close(&g);
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii >= '1' && k->ascii <= '9') {
                        tab_show(&g, k->ascii - '1');
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii == ']' || k->ascii == '}') {
                        tab_show(&g, (cur_tab + 1) % ntabs);
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii == '[' || k->ascii == '{') {
                        tab_show(&g, (cur_tab + ntabs - 1) % ntabs);
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii == 'b' || k->ascii == 'B') {
                        mark_toggle();
                        /* If the list is what is on screen, it has just
                         * changed underneath us. */
                        if (!strcmp(cur_url, "about:bookmarks")) show_marks(&g);
                    }
                    else if (k->ascii == 'm' || k->ascii == 'M') {
                        navigate(&g, "about:bookmarks", 1);
                        gui_edit_set(&urledit, cur_url);
                    }
                    else if (k->ascii == 's' || k->ascii == 'S') {
                        /* The link under the pointer, or failing that the
                         * page itself -- which is what "save this" means when
                         * nothing in particular is being pointed at. */
                        char want[1024];
                        if (hover_link >= 0)
                            url_resolve(cur_url, hrefbuf + linkoff[hover_link],
                                        want, sizeof want);
                        else
                            snprintf(want, sizeof want, "%s", cur_url);
                        if (want[0] && strncmp(want, "about:", 6) != 0)
                            download_url(&g, want);
                    }
                    else if (k->ascii == '/') {
                        find_open = 1;
                        find_query[0] = 0;
                        find_cur = 0;
                        find_scan();
                    }
                    else if (k->ascii == 'l' || k->ascii == 'L') { url_focus = 1; urledit.cur = urledit.len; }
                    else if (k->ascii == 'n' || k->ascii == 'N') { show_net = !show_net; net_scroll = 0; }
                    break;
                }
                continue;
            }

            mouse_event_t *m = &e.m;
            if (m->wheel) {
                if (show_net) {
                    net_scroll += m->wheel * 3;
                    if (net_scroll < 0) net_scroll = 0;
                } else {
                    scroll -= m->wheel * g.fh * 3;
                }
                dirty = 1;
            }

            int was = hover_link;
            hover_link = show_net ? -1 : link_at(&g, m->x, m->y);
            if (hover_link != was) dirty = 1;

            if (m->pressed & MB_LEFT) {
                dirty = 1;
                int hit = 0;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (!gui_in(m->x, m->y, x, y, w, h)) continue;
                    hit = 1;
                    if (i == 0) {
                        go_back(&g);
                        gui_edit_set(&urledit, cur_url);
                    } else if (i == 1) {
                        go_fwd(&g);
                        gui_edit_set(&urledit, cur_url);
                    } else if (i == 2) {
                        char here[1024];
                        snprintf(here, sizeof here, "%s", cur_url);
                        navigate(&g, here, 0);
                    } else {
                        show_net = !show_net;
                        net_scroll = 0;
                    }
                }
                if (!hit && ntabs > 1 && m->y >= TOOL_H && m->y < TOOL_H + TAB_H) {
                    for (int i = 0; i < ntabs; i++) {
                        int x, y, w, h;
                        tab_rect(&g, i, &x, &y, &w, &h);
                        if (!gui_in(m->x, m->y, x, y, w, h)) continue;
                        tab_show(&g, i);
                        gui_edit_set(&urledit, cur_url);
                        hit = 1;
                        break;
                    }
                    hit = 1;                    /* the strip swallows the rest */
                }
                if (!hit && m->y < TOOL_H) {
                    url_focus = 1;
                    urledit.cur = urledit.len;
                    hit = 1;
                }
                if (!hit && show_net) { url_focus = 0; hit = 1; }
                if (!hit && !show_net) {
                    /* A field is what the click was for, before any link
                     * under it and before the page's own handler runs on the
                     * element it happens to sit in. */
                    int fi = field_at(&g, m->x, m->y);
                    if (fi >= 0) {
                        field_t *f = &fields[fi];
                        url_focus = 0;
                        hit = 1;
                        if (f->disabled) {
                            /* nothing to do, and saying so beats silence */
                            snprintf(status, sizeof status, "that field is disabled");
                        } else if (f->kind == FLD_CHECK) {
                            f->checked = !f->checked;
                            focus_field = fi;
                            jsd_dispatch(f->node, "change");
                            reflow(&g);
                        } else if (f->kind == FLD_RADIO) {
                            /* One of a name at a time, within its own form. */
                            for (int k = 0; k < nfields; k++)
                                if (fields[k].kind == FLD_RADIO &&
                                    fields[k].form == f->form &&
                                    ci_eq(fields[k].name, f->name))
                                    fields[k].checked = 0;
                            f->checked = 1;
                            focus_field = fi;
                            jsd_dispatch(f->node, "change");
                            reflow(&g);
                        } else if (f->kind == FLD_SELECT) {
                            /* No menu to drop down, so a press is the next
                             * choice round. */
                            if (f->nopt > 0) f->opt = (f->opt + 1) % f->nopt;
                            focus_field = fi;
                            jsd_dispatch(f->node, "change");
                            reflow(&g);
                        } else if (f->kind == FLD_SUBMIT) {
                            focus_field = fi;
                            int nd = f->node;
                            int inform = f->form;
                            int handled = jsd_dispatch(nd, "click");
                            reflow(&g);
                            /* The page's own handler may have rebuilt the
                             * form, and then this index is somebody else's.
                             * The element it came from is what survives. */
                            fi = field_of_node(nd);
                            if (inform >= 0) {
                                if (fi >= 0) submit_form(&g, fi);
                            } else if (!handled) {
                                /* No form to send: if it stands inside a
                                 * link, that is what pressing it meant. */
                                int lk = link_around(nd);
                                if (lk >= 0) {
                                    char next[1024];
                                    url_resolve(cur_url, hrefbuf + linkoff[lk],
                                                next, sizeof next);
                                    focus_field = -1;
                                    navigate(&g, next, 1);
                                }
                            }
                            gui_edit_set(&urledit, cur_url);
                        } else if (f->kind == FLD_BUTTON) {
                            focus_field = fi;
                            int nd = f->node;
                            int handled = jsd_dispatch(nd, "click");
                            reflow(&g);
                            /* A button with nothing of its own to do, standing
                             * inside a link, is a link. */
                            if (!handled) {
                                int lk = link_around(nd);
                                if (lk >= 0) {
                                    char next[1024];
                                    url_resolve(cur_url, hrefbuf + linkoff[lk],
                                                next, sizeof next);
                                    focus_field = -1;
                                    navigate(&g, next, 1);
                                    gui_edit_set(&urledit, cur_url);
                                }
                            }
                        } else {
                            focus_field = fi;
                            /* The caret lands where it was pressed. */
                            int vx = MARGIN;
                            for (int i = 0; i < nitems; i++)
                                if (items[i].kind == IT_FIELD && items[i].off == fi) {
                                    int col = (m->x - (vx + items[i].x) - g.fw) / g.fw;
                                    int len = (int)strlen(f->value);
                                    f->cur = col < 0 ? 0 : (col > len ? len : col);
                                    break;
                                }
                        }
                    }
                }
                if (!hit) {
                    focus_field = -1;
                    url_focus = 0;
                    /* The page hears the click first. If something on it
                     * handled the click, that is what the click was for, and
                     * a link underneath is not followed as well. */
                    int node = node_at(&g, m->x, m->y);
                    int handled = (node >= 0) ? jsd_dispatch(node, "click") : 0;
                    reflow(&g);
                    if (!handled && hover_link >= 0) {
                        char next[1024];
                        url_resolve(cur_url, hrefbuf + linkoff[hover_link],
                                    next, sizeof next);
                        navigate(&g, next, 1);
                        gui_edit_set(&urledit, cur_url);
                        hover_link = -1;
                    }
                }
            }
        }

        /* Anything the page asked to happen later. */
        if (jsd_run_timers(uptime_ms())) {
            reflow(&g);
            dirty = 1;
        }

        {
            int page = g.h - VIEW_TOP - STAT_H - MARGIN;
            int maxs = doc_height - page;
            if (maxs < 0) maxs = 0;
            if (scroll > maxs) scroll = maxs;
            if (scroll < 0) scroll = 0;
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g);
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;
            }
        }

        /* One picture per pass, after the page has been drawn at least once.
         * Fetching them all before the first paint would leave a blank window
         * for as long as the slowest of them takes. */
        {
            int slot = pic_pending();
            if (slot >= 0) {
                int done = 0;
                for (int i = 0; i < npics; i++) if (pics[i].state != PIC_WANT) done++;
                snprintf(status, sizeof status, "pictures %d/%d ...", done + 1, npics);
                dirty = 1;
                if (gui_sync(&g)) { draw(&g); gui_present(&g); dirty = 0; }

                pic_load(&g, slot);
                layout(&g, g.w - 2 * MARGIN - 10);
                if (pic_pending() < 0) {
                    int ok = 0;
                    for (int i = 0; i < npics; i++) if (pics[i].state == PIC_OK) ok++;
                    if (npics && ok == npics)
                        snprintf(status, sizeof status, "%d pictures", ok);
                    else if (npics)
                        snprintf(status, sizeof status, "%d of %d pictures (%s)",
                                 ok, npics, pic_err[0] ? pic_err : "no reason given");
                }
                dirty = 1;
                continue;                   /* redraw before the next one */
            }
        }

        sleep_ms(20);
    }

    gui_close(&g);
    return 0;
}
