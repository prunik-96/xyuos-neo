/* Which font draws which character.
 *
 * No one font has every character, and none tries to: Noto is a family of a
 * hundred-odd fonts precisely so that each can be drawn by people who know
 * that script. So every style has an order to try them in -- its chain --
 * and a character is drawn by the first font in the chain that has it:
 *
 *   the face the style asked for        Noto Sans / Serif / Sans Mono
 *   Noto Sans, for a serif or mono face that lacks a character it has
 *   the scripts                         Arabic, Hebrew, Devanagari, Thai,
 *                                       Armenian, Georgian
 *   the symbols                         arrows, dingbats, mathematics
 *   Chinese, Japanese and Korean        one face, 16 MiB, so it is last
 *                                       among the letters and is read only
 *                                       when a page actually needs it
 *   emoji
 *
 * A weight or slant the file does not have is made from the one it does:
 * FreeType can thicken an outline and slant it. That is what a browser does
 * too when a page asks for bold Georgian.
 *
 * Fonts are read on first use, whole, into memory. FreeType and HarfBuzz both
 * work from those same bytes.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "txt_internal.h"
#include "linebreak.h"
#include "graphemebreak.h"

FT_Library txt_ftlib;
hb_unicode_funcs_t *txt_ucd;

enum {
    SANS_R, SANS_B, SANS_I, SANS_BI,
    SERIF_R, SERIF_B, SERIF_I, SERIF_BI,
    MONO_R, MONO_B,
    ARABIC_R, ARABIC_B, HEBREW_R, HEBREW_B, DEVA_R, DEVA_B, THAI_R, THAI_B,
    ARMENIAN, GEORGIAN,
    /* One regular face each, in the order they are tried. */
    FIRST_SCRIPT,
    BENGALI = FIRST_SCRIPT, GURMUKHI, GUJARATI, ORIYA, TAMIL, TELUGU, KANNADA,
    MALAYALAM, SINHALA, MYANMAR, KHMER, LAO, TIBETAN, ETHIOPIC, THAANA,
    SYRIAC, MONGOLIAN, CHEROKEE, TIFINAGH, NKO, OLCHIKI, JAVANESE, CANADIAN,
    LAST_SCRIPT = CANADIAN,
    SYMBOLS, SYMBOLS2, MATH, CJK, EMOJI,
    NFACES
};

static txt_face faces[NFACES] = {
    [SANS_R]   = { "NotoSans-Regular.ttf" },
    [SANS_B]   = { "NotoSans-Bold.ttf" },
    [SANS_I]   = { "NotoSans-Italic.ttf" },
    [SANS_BI]  = { "NotoSans-BoldItalic.ttf" },
    [SERIF_R]  = { "NotoSerif-Regular.ttf" },
    [SERIF_B]  = { "NotoSerif-Bold.ttf" },
    [SERIF_I]  = { "NotoSerif-Italic.ttf" },
    [SERIF_BI] = { "NotoSerif-BoldItalic.ttf" },
    [MONO_R]   = { "NotoSansMono-Regular.ttf" },
    [MONO_B]   = { "NotoSansMono-Bold.ttf" },
    [ARABIC_R] = { "NotoSansArabic-Regular.ttf" },
    [ARABIC_B] = { "NotoSansArabic-Bold.ttf" },
    [HEBREW_R] = { "NotoSansHebrew-Regular.ttf" },
    [HEBREW_B] = { "NotoSansHebrew-Bold.ttf" },
    [DEVA_R]   = { "NotoSansDevanagari-Regular.ttf" },
    [DEVA_B]   = { "NotoSansDevanagari-Bold.ttf" },
    [THAI_R]   = { "NotoSansThai-Regular.ttf" },
    [THAI_B]   = { "NotoSansThai-Bold.ttf" },
    [ARMENIAN] = { "NotoSansArmenian-Regular.ttf" },
    [GEORGIAN] = { "NotoSansGeorgian-Regular.ttf" },
    [BENGALI]  = { "NotoSansBengali-Regular.ttf" },
    [GURMUKHI] = { "NotoSansGurmukhi-Regular.ttf" },
    [GUJARATI] = { "NotoSansGujarati-Regular.ttf" },
    [ORIYA]    = { "NotoSansOriya-Regular.ttf" },
    [TAMIL]    = { "NotoSansTamil-Regular.ttf" },
    [TELUGU]   = { "NotoSansTelugu-Regular.ttf" },
    [KANNADA]  = { "NotoSansKannada-Regular.ttf" },
    [MALAYALAM] = { "NotoSansMalayalam-Regular.ttf" },
    [SINHALA]  = { "NotoSansSinhala-Regular.ttf" },
    [MYANMAR]  = { "NotoSansMyanmar-Regular.ttf" },
    [KHMER]    = { "NotoSansKhmer-Regular.ttf" },
    [LAO]      = { "NotoSansLao-Regular.ttf" },
    [TIBETAN]  = { "NotoSerifTibetan-Regular.ttf" },
    [ETHIOPIC] = { "NotoSansEthiopic-Regular.ttf" },
    [THAANA]   = { "NotoSansThaana-Regular.ttf" },
    [SYRIAC]   = { "NotoSansSyriac-Regular.ttf" },
    [MONGOLIAN] = { "NotoSansMongolian-Regular.ttf" },
    [CHEROKEE] = { "NotoSansCherokee-Regular.ttf" },
    [TIFINAGH] = { "NotoSansTifinagh-Regular.ttf" },
    [NKO]      = { "NotoSansNKo-Regular.ttf" },
    [OLCHIKI]  = { "NotoSansOlChiki-Regular.ttf" },
    [JAVANESE] = { "NotoSansJavanese-Regular.ttf" },
    [CANADIAN] = { "NotoSansCanadianAboriginal-Regular.ttf" },
    [SYMBOLS]  = { "NotoSansSymbols-Regular.ttf" },
    [SYMBOLS2] = { "NotoSansSymbols2-Regular.ttf" },
    [MATH]     = { "NotoSansMath-Regular.ttf" },
    [CJK]      = { "NotoSansCJKsc-Regular.otf" },
    [EMOJI]    = { "NotoEmoji-Regular.ttf" },
};

static char dir[160] = "/fonts";
static int ready;
static txt_chain chains[3][2][2];

const char *txt_face_name(int face) {
    return (face >= 0 && face < NFACES) ? faces[face].file : "?";
}

int txt_faces_loaded(void) {
    int n = 0;
    for (int i = 0; i < NFACES; i++) n += faces[i].state > 0;
    return n;
}

const char *txt_glyph_name(int face, unsigned gid) {
    static char name[64];
    txt_face *f = txt_boot() ? txt_face_get(face) : NULL;
    if (!f || !FT_HAS_GLYPH_NAMES(f->ft) ||
        FT_Get_Glyph_Name(f->ft, gid, name, sizeof name))
        return "";
    return name;
}

/* --- loading -------------------------------------------------------------- */

static unsigned char *slurp(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    long end = lseek(fd, 0, SEEK_END);
    if (end <= 0 || lseek(fd, 0, SEEK_SET) != 0) { close(fd); return NULL; }
    unsigned char *p = malloc((size_t)end);
    if (!p) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)end) {
        long n = read(fd, p + got, (size_t)end - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != (size_t)end) { free(p); return NULL; }
    *size = got;
    return p;
}

txt_face *txt_face_get(int i) {
    if (i < 0 || i >= NFACES) return NULL;
    txt_face *f = &faces[i];
    if (f->state > 0) return f;
    if (f->state < 0) return NULL;
    f->state = -1;                        /* until it has actually worked */

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, f->file);
    f->data = slurp(path, &f->size);
    if (!f->data) return NULL;

    if (FT_New_Memory_Face(txt_ftlib, f->data, (FT_Long)f->size, 0, &f->ft)) {
        free(f->data);
        f->data = NULL;
        return NULL;
    }
    hb_blob_t *blob = hb_blob_create((const char *)f->data, (unsigned)f->size,
                                     HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *hf = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    f->upem = (int)hb_face_get_upem(hf);
    f->hb = hb_font_create(hf);           /* scale defaults to upem */
    hb_face_destroy(hf);
    if (f->upem <= 0) f->upem = f->ft->units_per_EM ? f->ft->units_per_EM : 1000;
    f->state = 1;
    return f;
}

/* --- what each font has, without opening it ------------------------------ */

/* tools/fontindex.py writes down, at build time, which characters each font
 * maps to a real glyph: /fonts/index, one line per font, ranges in hex. With
 * it, deciding that a font does not have a character costs a binary search.
 * Without it, the question has to be put to the font itself, which means
 * reading it -- and a character nothing has would read all of them. */
typedef struct { uint32_t lo, hi; } range_t;

static range_t *cover[NFACES];
static int      ncover[NFACES];
static int      indexed;        /* 0 not tried, 1 read, -1 there is none */

static int face_named(const char *s, size_t n) {
    for (int i = 0; i < NFACES; i++)
        if (strlen(faces[i].file) == n && memcmp(faces[i].file, s, n) == 0)
            return i;
    return -1;
}

static uint32_t hex(const char **p, const char *end) {
    uint32_t v = 0;
    for (; *p < end; (*p)++) {
        char c = **p;
        if (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
        else break;
    }
    return v;
}

static void read_index(void) {
    indexed = -1;
    char path[256];
    snprintf(path, sizeof path, "%s/index", dir);
    size_t size;
    char *text = (char *)slurp(path, &size);
    if (!text) return;

    const char *s = text, *end = text + size;
    while (s < end) {
        const char *eol = memchr(s, '\n', (size_t)(end - s));
        if (!eol) eol = end;
        const char *sp = memchr(s, ' ', (size_t)(eol - s));
        int f = sp ? face_named(s, (size_t)(sp - s)) : -1;
        if (f >= 0 && !cover[f]) {
            int n = 0;
            for (const char *q = sp; q < eol; q++) n += *q == ' ';
            cover[f] = malloc((size_t)n * sizeof *cover[f]);
            const char *q = sp;
            while (cover[f] && q < eol && ncover[f] < n) {
                q++;                                    /* the space */
                range_t r;
                r.lo = hex(&q, eol);
                if (q < eol && *q == '-') q++;
                r.hi = hex(&q, eol);
                cover[f][ncover[f]++] = r;
            }
        }
        s = eol + 1;
    }
    free(text);
    indexed = 1;
}

int txt_has(int face, uint32_t cp) {
    if (!indexed) read_index();
    if (indexed > 0) {
        /* A font the index does not list was not on the disk it was made
         * from: as good as absent. */
        const range_t *r = cover[face];
        int lo = 0, hi = r ? ncover[face] - 1 : -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (cp < r[mid].lo) hi = mid - 1;
            else if (cp > r[mid].hi) lo = mid + 1;
            else return 1;
        }
        return 0;
    }
    txt_face *f = txt_face_get(face);
    return f && FT_Get_Char_Index(f->ft, cp) != 0;
}

/* --- the chains ----------------------------------------------------------- */

static void add(txt_chain *c, int face, int bold, int italic) {
    if (c->n >= TXT_SLOTS_MAX) return;
    c->s[c->n].face = (short)face;
    c->s[c->n].bold = (char)bold;
    c->s[c->n].italic = (char)italic;
    c->n++;
}

/* A script font: its bold file when there is one, else the regular made
 * bold. None of them has an italic, so a slant is always made. */
static void add_script(txt_chain *c, int regular, int boldfile, int b, int i) {
    if (b && boldfile >= 0) add(c, boldfile, 0, i);
    else add(c, regular, b, i);
}

static void build(void) {
    for (int f = 0; f < 3; f++)
    for (int b = 0; b < 2; b++)
    for (int i = 0; i < 2; i++) {
        txt_chain *c = &chains[f][b][i];
        c->id = f * 4 + b * 2 + i;
        c->n = 0;
        int sans = SANS_R + i * 2 + b;        /* R, B, I, BI in that order */
        if (f == TXT_SERIF) add(c, SERIF_R + i * 2 + b, 0, 0);
        if (f == TXT_MONO)  add(c, MONO_R + b, 0, i);
        add(c, sans, 0, 0);
        add_script(c, ARABIC_R, ARABIC_B, b, i);
        add_script(c, HEBREW_R, HEBREW_B, b, i);
        add_script(c, DEVA_R, DEVA_B, b, i);
        add_script(c, THAI_R, THAI_B, b, i);
        add_script(c, ARMENIAN, -1, b, i);
        add_script(c, GEORGIAN, -1, b, i);
        for (int s = FIRST_SCRIPT; s <= LAST_SCRIPT; s++)
            add_script(c, s, -1, b, i);
        add_script(c, SYMBOLS, -1, b, i);
        add_script(c, SYMBOLS2, -1, b, i);
        add_script(c, MATH, -1, b, i);
        add_script(c, CJK, -1, b, i);
        add(c, EMOJI, 0, 0);                  /* a thickened emoji is a blot */
    }
}

const txt_chain *txt_chain_for(const txt_style *st) {
    int f = (st->family >= TXT_SANS && st->family <= TXT_MONO) ? st->family : TXT_SANS;
    return &chains[f][st->weight >= 600][st->italic != 0];
}

/* The same few hundred characters are asked about over and over -- a page is
 * mostly one alphabet -- so the answers are remembered. Direct-mapped: a
 * collision costs one more lookup, never a wrong answer. */
#define PICK_CACHE 2048
static struct { uint32_t cp; short chain, slot; } picked[PICK_CACHE];

int txt_pick(const txt_chain *c, uint32_t cp) {
    unsigned h = ((cp * 2654435761u) ^ ((unsigned)c->id * 40503u)) & (PICK_CACHE - 1);
    if (picked[h].chain == c->id + 1 && picked[h].cp == cp) return picked[h].slot;
    int r = -1;
    for (int k = 0; k < c->n; k++)
        if (txt_has(c->s[k].face, cp)) { r = k; break; }
    picked[h].cp = cp;
    picked[h].chain = (short)(c->id + 1);
    picked[h].slot = (short)r;
    return r;
}

/* --- the public part ------------------------------------------------------ */

int txt_boot(void) {
    if (!ready) {
        if (FT_Init_FreeType(&txt_ftlib)) return 0;
        txt_ucd = hb_unicode_funcs_get_default();
        build();
        init_linebreak();
        init_graphemebreak();
        ready = 1;
    }
    return 1;
}

int txt_init(const char *d) {
    if (d && *d && strlen(d) < sizeof dir) strcpy(dir, d);
    if (!txt_boot()) return 0;
    int found = 0;
    char path[256];
    for (int i = 0; i < NFACES; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, faces[i].file);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) { found++; close(fd); }
    }
    return found;
}

void txt_metrics(const txt_style *st, int *ascent, int *descent) {
    txt_face *f = txt_boot() ? txt_face_get(txt_chain_for(st)->s[0].face) : NULL;
    int a = 0, d = 0;
    if (f) {
        a = txt_scale(f->ft->ascender, st->size, f->upem);
        d = txt_scale(-f->ft->descender, st->size, f->upem);
    } else {
        a = st->size * 4 / 5;
        d = st->size / 5;
    }
    if (ascent)  *ascent = (a + 63) >> 6;
    if (descent) *descent = (d + 63) >> 6;
}
