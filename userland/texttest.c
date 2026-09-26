/* libtext, checked the way it will be used: on real fonts, real scripts.
 *
 * Each check asks for something a page actually contains and says what it
 * found, by glyph name where the font keeps names -- so "one glyph" is not
 * taken on trust, the output shows it is f_i and not a stray f.
 *
 * What is printed here is ASCII on purpose: this program's own output goes
 * to a terminal whose font is exactly the kind this layer replaces.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "text.h"

static int bad;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) bad++;
}

static txt_style style(txt_family f, int px) {
    txt_style s = { f, 400, 0, px * 64 };
    return s;
}

#define MAXG 64
static txt_glyph g[MAXG];

static int shape(const txt_style *st, const char *s) {
    int n = txt_shape_info(st, s, strlen(s), g, MAXG);
    return n > MAXG ? MAXG : n;
}

/* Glyph names, each in its own buffer: txt_glyph_name reuses one, and two
 * calls in the same printf would otherwise print the second name twice. */
static char names[4][64];
static const char *name(int i) {
    char *b = names[i & 3];
    strncpy(b, txt_glyph_name(g[i].face, g[i].gid), 63);
    b[63] = 0;
    return b;
}
static const char *face(int i) { return txt_face_name(g[i].face); }

/* Right to left: going across the glyphs from the left, the text they came
 * from only goes back. Glyphs of one letter -- a base and the dot a font
 * draws separately -- share its offset, so equal is allowed. */
static int descending(int n) {
    for (int i = 1; i < n; i++) if (g[i].cluster > g[i - 1].cluster) return 0;
    return n > 1 && g[0].cluster > g[n - 1].cluster;
}

/* The letters themselves, leaving out the marks placed on them: a mark is
 * the glyph that takes no room. Returns how many, gids in `out`. */
static int bases(int n, unsigned *out, int max) {
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (g[i].x_advance > 0) out[k++] = g[i].gid;
    return k;
}

int main(void) {
    int n, found = txt_init(NULL), named = 0;
    while (strcmp(txt_face_name(named), "?") != 0) named++;
    printf("-- text: fonts, shaping, bidi, line breaking --\n");
    printf("   %d of %d font files in /fonts\n", found, named);
    check(found == named, "every font the chains name is on the disk");

    txt_style sans = style(TXT_SANS, 32), serif = style(TXT_SERIF, 32);

    /* Fonts are read only when they draw something. A character no font has
     * must not read them all to find that out: that is what /fonts/index is
     * for. */
    txt_width(&sans, "Hello", 5);
    int after_latin = txt_faces_loaded();
    txt_width(&sans, "\xF4\x8F\xBF\xBD", 4);                     /* U+10FFFD */
    printf("   fonts in memory: %d after Latin, %d after a character none has\n",
           after_latin, txt_faces_loaded());
    check(after_latin == 1 && txt_faces_loaded() == 1,
          "a missing character is looked up in the index, not in every font");

    /* Kerning: A and V tuck into each other. */
    int a = txt_width(&sans, "A", 1), v = txt_width(&sans, "V", 1);
    int av = txt_width(&sans, "AV", 2);
    printf("   A %d + V %d = %d, AV %d px\n", a, v, a + v, av);
    check(av < a + v, "kerning: AV is narrower than A and V apart");

    /* Ligature. */
    n = shape(&serif, "fi");
    printf("   serif \"fi\": %d glyph(s): %s\n", n, n ? name(0) : "");
    check(n == 1, "ligature: fi is one glyph in Noto Serif");

    /* A combining accent with no precomposed form is placed by the font. */
    n = shape(&sans, "x\xCC\x81");                  /* x + U+0301 */
    printf("   x + acute: %d glyphs, mark %s advance %d offset %d,%d\n",
           n, n > 1 ? name(1) : "-", n > 1 ? g[1].x_advance : 0,
           n > 1 ? g[1].x_offset : 0, n > 1 ? g[1].y_offset : 0);
    check(n == 2 && g[1].x_advance == 0 && (g[1].x_offset || g[1].y_offset),
          "a combining mark takes no room and sits over its letter");

    /* ...and one that has a precomposed form becomes it. */
    n = shape(&sans, "e\xCC\x81");                  /* e + U+0301 */
    printf("   e + acute: %d glyph: %s\n", n, n ? name(0) : "");
    check(n == 1, "e + combining acute is drawn as the one letter e-acute");

    /* Arabic: the same letter three ways, joined, right to left. Noto draws
     * beh as a dotless body plus a dot below, so the letters are the glyphs
     * with width and the dots are marks on them. */
    unsigned beh[4], alone[2];
    n = shape(&sans, "\xD8\xA8\xD8\xA8\xD8\xA8");   /* beh beh beh */
    int nb = bases(n, beh, 4);
    printf("   beh beh beh in %s: %d glyphs, letters", n ? face(0) : "-", n);
    for (int i = 0; i < n; i++)
        if (g[i].x_advance > 0) printf(" %s", name(i));
    printf("\n");
    check(nb == 3 && beh[0] != beh[1] && beh[1] != beh[2] && beh[0] != beh[2],
          "Arabic: final, medial and initial forms are three different glyphs");
    check(descending(n), "Arabic runs right to left");
    n = shape(&sans, "\xD8\xA8");
    printf("   beh alone:");
    for (int i = 0; i < n; i++)
        if (g[i].x_advance > 0) printf(" %s", name(i));
    printf("\n");
    check(bases(n, alone, 2) == 1 && alone[0] != beh[0] && alone[0] != beh[1] &&
          alone[0] != beh[2], "a beh on its own is the fourth, isolated form");

    /* Hebrew, and a sentence that is both. */
    n = shape(&sans, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");   /* shalom */
    printf("   shalom in %s, %d glyphs\n", n ? face(0) : "-", n);
    check(n == 4 && descending(n), "Hebrew runs right to left");
    n = shape(&sans, "abc \xD7\x90\xD7\x91\xD7\x92");       /* abc + alef bet gimel */
    printf("   \"abc ABG\": clusters");
    for (int i = 0; i < n; i++) printf(" %u", g[i].cluster);
    printf("\n");
    check(n == 7 && g[0].cluster == 0 && g[1].cluster == 1 && g[2].cluster == 2 &&
          g[4].cluster > g[5].cluster && g[5].cluster > g[6].cluster,
          "bidi: Latin left to right, then the Hebrew after it right to left");
    n = shape(&sans, "\xD8\xB9\xD8\xAF\xD8\xAF 123");      /* 'adad 123 */
    printf("   Arabic + 123: clusters");
    for (int i = 0; i < n; i++) printf(" %u", g[i].cluster);
    printf("\n");
    check(n >= 6 && g[0].cluster < g[1].cluster && g[1].cluster < g[2].cluster,
          "bidi: digits inside Arabic still read left to right, and come first");

    /* Devanagari: the i-sign is written after KA and drawn before it. */
    n = shape(&sans, "\xE0\xA4\x95");                       /* KA */
    unsigned ka = g[0].gid;
    n = shape(&sans, "\xE0\xA4\x95\xE0\xA4\xBF");           /* KA + I */
    printf("   KA + I in %s: %s %s\n", n ? face(0) : "-", n > 0 ? name(0) : "",
           n > 1 ? name(1) : "");
    check(n == 2 && g[1].gid == ka && g[0].gid != ka,
          "Devanagari: the vowel sign i moves in front of its consonant");
    n = shape(&sans, "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7"); /* KA virama SSA */
    printf("   KA virama SSA: %d glyph: %s\n", n, n ? name(0) : "");
    check(n == 1, "Devanagari: KA + SSA join into the conjunct KSSA");

    /* Thai, whose vowel and tone marks stack on the consonant. */
    n = shape(&sans, "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5");
    printf("   sawasdi in %s, %d glyphs\n", n ? face(0) : "-", n);
    check(n == 6 && strstr(face(0), "Thai") != NULL, "Thai goes to the Thai face");

    /* The scripts of South Asia beyond Devanagari. */
    n = shape(&sans, "\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\x82\xE0\xA6\xB2\xE0\xA6\xBE");  /* bangla */
    printf("   bangla in %s\n", n ? face(0) : "-");
    check(n > 0 && strstr(face(0), "Bengali") != NULL, "Bengali goes to the Bengali face");
    n = shape(&sans, "\xE0\xAE\xA4\xE0\xAE\xAE\xE0\xAE\xBF\xE0\xAE\xB4\xE0\xAF\x8D");  /* tamizh */
    printf("   tamil in %s\n", n ? face(0) : "-");
    check(n > 0 && strstr(face(0), "Tamil") != NULL, "Tamil goes to the Tamil face");

    /* Choosing fonts: one string, five faces. */
    n = shape(&sans, "A\xD1\x8F\xCE\xB1\xE4\xBD\xA0\xF0\x9F\x98\x80");  /* A ya alpha ni grin */
    printf("   A ya alpha ni grin:");
    for (int i = 0; i < n; i++) printf(" %s", face(i));
    printf("\n");
    check(n == 5 && strcmp(face(0), "NotoSans-Regular.ttf") == 0 &&
          strcmp(face(1), face(0)) == 0 && strcmp(face(2), face(0)) == 0 &&
          strstr(face(3), "CJK") && strstr(face(4), "Emoji"),
          "fallback: Latin, Cyrillic, Greek from Noto Sans; Chinese and emoji elsewhere");
    txt_style bold = sans;
    bold.weight = 700;
    n = shape(&bold, "A\xE4\xBD\xA0");
    check(n == 2 && strcmp(face(0), "NotoSans-Bold.ttf") == 0 && strstr(face(1), "CJK"),
          "bold asks for the bold file, and the CJK face (no bold) is thickened");

    /* Line breaking. */
    int at = 0;
    const char *words = "hello world again";
    int hw = txt_width(&sans, "hello world", 11);
    size_t sp = txt_split(&sans, words, strlen(words), hw + 5, &at);
    printf("   split \"%s\" at %d px: offset %u, width %d (\"hello world\" is %d)\n",
           words, hw + 5, (unsigned)sp, at, hw);
    check(sp == 11 && at == hw, "a line ends at the last space that still fits");
    sp = txt_split(&sans, words, strlen(words), 3, &at);
    check(sp == 5, "a first word too long for the line overflows, it is not cut");
    const char *long_word = "internationalisation";
    sp = txt_split(&sans, long_word, strlen(long_word), 20, &at);
    check(sp == strlen(long_word), "one unbreakable word is never split");
    const char *zh = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"
                     "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";   /* ni hao shi jie x2 */
    int zw = txt_width(&sans, zh, strlen(zh));
    sp = txt_split(&sans, zh, strlen(zh), zw / 2, &at);
    printf("   Chinese, %d px, split at half: offset %u of %u, width %d\n",
           zw, (unsigned)sp, (unsigned)strlen(zh), at);
    check(sp > 0 && sp < strlen(zh) && sp % 3 == 0 && at <= zw / 2,
          "Chinese breaks between characters, no spaces needed");

    /* Hitting: never inside a character. */
    const char *ea = "e\xCC\x81x";                               /* e + acute, x */
    int ew = txt_width(&sans, "e\xCC\x81", 3);
    size_t h = txt_hit(&sans, ea, strlen(ea), ew / 2 + 1, &at);
    printf("   hit at %d in e-acute-x: offset %u, x %d\n", ew / 2 + 1, (unsigned)h, at);
    check(h == 0 || h == 3, "a click never lands between e and its accent");

    /* What is measured is what is drawn. */
    static uint32_t px[200 * 60];
    for (int i = 0; i < 200 * 60; i++) px[i] = 0xFFFFFF;
    txt_target t = { px, 200, 0, 0, 200, 60 };
    txt_style s20 = style(TXT_SANS, 20);
    /* Not a word ending in f: its hook hangs past its advance by design. */
    const char *word = "Hello";
    int ww = txt_width(&s20, word, 5);
    txt_draw(&t, &s20, 10, 40, 0x000000, word, 5);
    int minx = 999, maxx = -1, inked = 0;
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < 200; x++)
            if (px[y * 200 + x] != 0xFFFFFF) {
                inked++;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
            }
    printf("   \"Hello\" measured %d px, ink from %d to %d (%d pixels)\n",
           ww, minx, maxx, inked);
    check(inked > 50 && minx >= 10 && maxx < 10 + ww + 2,
          "the ink of a drawn word stays inside its measured width");

    /* How fast. */
    char buf[32];
    unsigned t0 = uptime_ms();
    for (int i = 0; i < 2000; i++) {
        snprintf(buf, sizeof buf, "word%d and more", i);
        txt_width(&sans, buf, strlen(buf));
    }
    unsigned t1 = uptime_ms();
    printf("   2000 different strings measured in %u ms\n", t1 - t0);

    printf(bad ? "%d FAILED\n" : "all good\n", bad);
    return bad != 0;
}
