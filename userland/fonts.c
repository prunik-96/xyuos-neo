/* A window of text in every script the system has fonts for.
 *
 * texttest checks the text stack by numbers; this is for the eye. Kerning
 * that is measured but looks wrong, a mark that sits a pixel too high, a
 * weight that reads as a smudge -- none of those fail a check, and all of
 * them are obvious here.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"
#include "text.h"

typedef struct {
    txt_family family;
    int px, weight, italic;
    const char *text;
} line_t;

static const line_t LINES[] = {
    { TXT_SANS, 16, 400, 0, "The quick brown fox jumps over the lazy dog. \xC2\xAB\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB8\xC2\xBB \xE2\x80\x94 \xE2\x80\x9Cquotes\xE2\x80\x9D, 1\xE2\x80\x93" "2, wait\xE2\x80\xA6 \xE2\x80\xA2 bullet" },
    { TXT_SERIF, 20, 400, 0, "Office affinity, AVATAR, Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t, \xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC, \xD0\xA1\xD1\x8A\xD0\xB5\xD1\x88\xD1\x8C \xD0\xB6\xD0\xB5 \xD0\xB5\xD1\x89\xD1\x91" },
    { TXT_MONO, 15, 400, 0, "int main(void) { return 0; }   // 0O 1lI |" },
    { TXT_SANS, 24, 400, 0, "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 \xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85 \xE2\x80\x94 \xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 2026" },
    { TXT_SANS, 22, 400, 0, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D \xC2\xB7 HTML \xD7\xA0\xD7\x95\xD7\xA6\xD7\xA8 \xD7\x91\xD6\xBE" "1991 and CSS" },
    { TXT_SANS, 24, 400, 0, "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87 \xE0\xA4\xA6\xE0\xA5\x81\xE0\xA4\xA8\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA4\xBE \xE2\x80\x94 \xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\xAF" },
    { TXT_SANS, 22, 400, 0, "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5\xE0\xB8\x8A\xE0\xB8\xB2\xE0\xB8\xA7\xE0\xB9\x82\xE0\xB8\xA5\xE0\xB8\x81 \xC2\xB7 \xE1\x83\xA5\xE1\x83\x90\xE1\x83\xA0\xE1\x83\x97\xE1\x83\xA3\xE1\x83\x9A\xE1\x83\x98 \xC2\xB7 \xD5\x80\xD5\xA1\xD5\xB5\xD5\xA5\xD6\x80\xD5\xA5\xD5\xB6" },
    { TXT_SANS, 22, 400, 0, "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C \xC2\xB7 \xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF \xC2\xB7 \xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94" },
    { TXT_SANS, 22, 400, 0, "\xE2\x88\x80x\xE2\x88\x88\xE2\x84\x9D: x\xC2\xB2 \xE2\x89\xA5 0 \xC2\xB7 \xE2\x86\x90 \xE2\x86\x92 \xE2\x86\x91 \xE2\x86\x93 \xC2\xB7 \xE2\x9C\x93 \xE2\x98\x85 \xE2\x99\x9E \xC2\xB7 \xE2\x88\x91 \xE2\x88\xAB \xE2\x88\x9A \xC2\xB7 \xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 \xF0\x9F\x8C\x8D \xE2\x9D\xA4 \xF0\x9F\x91\x8D" },
};

static void draw(gui_t *g) {
    gui_clear(g, GC_PANEL);
    txt_target t = { g->px, g->w, 0, 0, g->w, g->h };
    int x = 24, y = 16;

    txt_style head = { TXT_SANS, 700, 0, 30 * 64 };
    int asc;
    txt_metrics(&head, &asc, NULL);
    y += asc;
    const char *title = "xyuOS \xE2\x80\x94 \xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82";   /* "text" */
    txt_draw(&t, &head, x, y, GC_TEXT, title, strlen(title));
    y += 18;

    for (unsigned i = 0; i < sizeof LINES / sizeof LINES[0]; i++) {
        const line_t *l = &LINES[i];
        txt_style st = { l->family, l->weight, l->italic, l->px * 64 };
        int a, d;
        txt_metrics(&st, &a, &d);
        y += a + 6;
        txt_draw(&t, &st, x, y, GC_TEXT, l->text, strlen(l->text));
        y += d + 6;
    }

    /* Weights and slants, one after another on a line. */
    static const struct { int weight, italic; const char *s; } styles[] = {
        { 400, 0, "\xD0\x9E\xD0\xB1\xD1\x8B\xD1\x87\xD0\xBD\xD1\x8B\xD0\xB9 " },
        { 700, 0, "\xD0\x96\xD0\xB8\xD1\x80\xD0\xBD\xD1\x8B\xD0\xB9 " },
        { 400, 1, "\xD0\x9A\xD1\x83\xD1\x80\xD1\x81\xD0\xB8\xD0\xB2 " },
        { 700, 1, "\xD0\x96\xD0\xB8\xD1\x80\xD0\xBD\xD1\x8B\xD0\xB9 \xD0\xBA\xD1\x83\xD1\x80\xD1\x81\xD0\xB8\xD0\xB2 " },
        { 700, 0, "\xE4\xB8\xAD\xE6\x96\x87 " },            /* made bold */
        { 400, 1, "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA" }, /* made slanted */
    };
    for (int fam = 0; fam < 2; fam++) {
        int cx = x;
        txt_style st = { fam ? TXT_SERIF : TXT_SANS, 400, 0, 20 * 64 };
        int a, d;
        txt_metrics(&st, &a, &d);
        y += a + 6;
        for (unsigned i = 0; i < sizeof styles / sizeof styles[0]; i++) {
            st.weight = styles[i].weight;
            st.italic = styles[i].italic;
            size_t n = strlen(styles[i].s);
            txt_draw(&t, &st, cx, y, GC_TEXT, styles[i].s, n);
            cx += txt_width(&st, styles[i].s, n);
        }
        y += d + 4;
    }

    /* A size ramp. */
    int cx = x;
    y += 44;
    static const int sizes[] = { 10, 12, 14, 16, 20, 24, 32, 44 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        txt_style st = { TXT_SANS, 400, 0, sizes[i] * 64 };
        char s[24];
        snprintf(s, sizeof s, "%dpx \xD0\x90\xD0\xB0 ", sizes[i]);
        txt_draw(&t, &st, cx, y, GC_TEXT, s, strlen(s));
        cx += txt_width(&st, s, strlen(s));
    }
}

int main(void) {
    if (txt_init(NULL) == 0) {
        printf("fonts: no fonts in /fonts\n");
        return 1;
    }
    gui_t g;
    if (!gui_open(&g)) return 1;

    int running = 1, dirty = 1;
    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type != GE_KEY) continue;
            if (e.k.code == XKEY_RESIZE) { gui_sync(&g); dirty = 1; }
            else if (e.k.code == XKEY_ESC ||
                     (e.k.code == XKEY_CHAR && e.k.ascii == 'q')) running = 0;
        }
        if (dirty) {
            if (gui_sync(&g)) {
                unsigned t0 = uptime_ms();
                draw(&g);
                gui_present(&g);
                printf("fonts: drawn in %u ms\n", uptime_ms() - t0);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;
            }
        }
        sleep_ms(30);
    }
    gui_close(&g);
    return 0;
}
