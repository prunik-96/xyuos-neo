/* view -- the viewer.
 *
 * One program for everything the system can display: PNG, JPEG and BMP
 * through img.h, and anything else as text. That is a deliberate choice --
 * the file manager's association table stays small because there is one
 * program to point at, and a file whose extension lies still opens, because
 * the format is decided by sniffing the magic bytes and not by the name.
 *
 * Downscaling averages the source box rather than picking one pixel out of
 * it. On a photo shrunk to a third that is the difference between an image
 * and a field of aliasing artefacts, and it costs one pass over the source. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui.h"
#include "img.h"
#include "upath.h"

#define TOOL_H 40
#define STAT_H 26

static char    path[UPATH_MAX];
static char    status[220];
static image_t im;
static int     is_text;

static char   *text;
static long    text_len;
static int     text_top, text_left, text_lines;

static void cache_drop(void);    /* defined with the scaler, below */

static int  zoom = 100;          /* percent; 0 = fit to window */
static int  panx, pany;
static int  fit = 1;

/* --- loading -------------------------------------------------------------- */

static void count_lines(void) {
    text_lines = 1;
    for (long i = 0; i < text_len; i++) if (text[i] == '\n') text_lines++;
}

static int load(const char *p) {
    cache_drop();
    img_free(&im);
    free(text);
    text = 0;
    text_len = 0;
    is_text = 0;
    text_top = text_left = 0;
    panx = pany = 0;
    fit = 1;
    zoom = 100;

    struct xyuos_stat st;
    if (xyuos_stat(p, &st) != 0) {
        snprintf(status, sizeof status, "cannot stat %s", p);
        return 0;
    }
    long fd = xyuos_open(p);
    if (fd < 0) {
        snprintf(status, sizeof status, "cannot open %s", p);
        return 0;
    }
    unsigned long n = st.size;
    unsigned char *data = (unsigned char *)malloc(n ? n : 1);
    if (!data) { xyuos_close(fd); snprintf(status, sizeof status, "out of memory"); return 0; }

    unsigned long got = 0;
    long r;
    while (got < n && (r = xyuos_read(fd, data + got, n - got)) > 0) got += (unsigned long)r;
    xyuos_close(fd);

    snprintf(path, sizeof path, "%s", p);

    if (img_load(data, got, &im)) {
        free(data);
        snprintf(status, sizeof status, "%s  --  %d x %d", p, im.w, im.h);
        return 1;
    }

    /* Not an image (or one we cannot read): fall back to showing the bytes.
     * Keep img_err around -- if the file really was a PNG we could not
     * decode, the user deserves to know why rather than see mojibake. */
    const char *why = img_err;
    int looks_image = got > 4 && (data[0] == 137 || (data[0] == 0xFF && data[1] == 0xD8) ||
                                  (data[0] == 'B' && data[1] == 'M'));
    is_text = 1;
    text = (char *)data;
    text_len = (long)got;
    count_lines();
    if (looks_image) snprintf(status, sizeof status, "%s  --  %s", p, why);
    else snprintf(status, sizeof status, "%s  --  %ld bytes, %d lines", p, text_len, text_lines);
    return 1;
}

/* --- directory walk (Prev / Next) ------------------------------------------ */

static char  dirpath[UPATH_MAX];
static char  siblings[128][64];
static int   nsib, sibidx;

static void collect_sibling(const char *name, int is_dir, void *ctx) {
    (void)ctx;
    if (is_dir || nsib >= 128) return;
    snprintf(siblings[nsib], 64, "%s", name);
    nsib++;
}

static void scan_dir(void) {
    nsib = 0;
    sibidx = -1;
    int cut = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/') cut = i;
    snprintf(dirpath, sizeof dirpath, "%.*s", cut ? cut : 1, path);

    static char buf[16384];
    long n = xyuos_listdir(dirpath, buf, sizeof buf);
    if (n > 0) upath_each_entry(buf, n, collect_sibling, 0);

    const char *base = path + (cut ? cut + 1 : 1);
    for (int i = 0; i < nsib; i++)
        if (strcmp(siblings[i], base) == 0) sibidx = i;
}

static void step(int dir) {
    if (nsib < 1) return;
    if (sibidx < 0) sibidx = 0;
    sibidx = (sibidx + dir + nsib) % nsib;
    char next[UPATH_MAX];
    upath_resolve(dirpath, siblings[sibidx], next);
    load(next);
}

/* --- image painting ------------------------------------------------------- */

static void view_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = TOOL_H;
    *w = g->w;
    *h = g->h - TOOL_H - STAT_H;
    if (*h < 0) *h = 0;
}

static int fit_zoom(gui_t *g) {
    int x, y, w, h;
    view_rect(g, &x, &y, &w, &h);
    if (im.w <= 0 || im.h <= 0 || w <= 0 || h <= 0) return 100;
    int zx = w * 100 / im.w, zy = h * 100 / im.h;
    int z = zx < zy ? zx : zy;
    if (z < 1) z = 1;
    if (z > 100) z = 100;             /* never blow a small image up to "fit" */
    return z;
}

static void clamp_pan(gui_t *g, int z) {
    int vx, vy, vw, vh;
    view_rect(g, &vx, &vy, &vw, &vh);
    int dw = im.w * z / 100, dh = im.h * z / 100;
    int maxx = dw - vw, maxy = dh - vh;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    if (panx > maxx) panx = maxx;
    if (pany > maxy) pany = maxy;
    if (panx < 0) panx = 0;
    if (pany < 0) pany = 0;
}

/* The visible part of the picture, already scaled.
 *
 * Rescaling a photograph costs one pass over its pixels. Doing that per frame
 * is fine while something is actually changing and ruinous when nothing is:
 * a blinking caret, a hovered button or a stray mouse event would each pay for
 * the whole image again. So the scaled result is kept, keyed by everything
 * that can change it, and an unchanged frame becomes a row copy.
 *
 * The cache is the size of the VIEWPORT, not of the scaled image: at 1600% a
 * full scaled copy of an 800x600 photo would be four hundred megabytes, while
 * what can actually be seen is never bigger than the window. */
static unsigned int *fc;                 /* viewport-sized cache */
static int fc_w, fc_h;                   /* its dimensions       */
static int fc_z, fc_px, fc_py, fc_valid; /* what it was built for */

static void cache_drop(void) { fc_valid = 0; }

/* The backdrop behind a picture, mixed from the current palette rather than
 * fixed. It has to be visible against a light image AND a dark one, so it is
 * the window colour nudged a little way towards the text colour -- which comes
 * out light grey in the light theme and dark grey in the dark one. */
static unsigned back_a(void) { return gui_mix(GC_WIN, GC_TEXT, 10); }
static unsigned back_b(void) { return gui_mix(GC_WIN, GC_TEXT, 22); }

/* Average the source box that lands on one destination pixel. For a shrink
 * that is the difference between a photograph and a field of aliasing; for a
 * magnification the box is a single pixel and this is just a lookup. */
static unsigned sample_box(int sx0, int sx1, int sy0, int sy1) {
    if (sx1 > im.w) sx1 = im.w;
    if (sy1 > im.h) sy1 = im.h;
    if (sx1 <= sx0) sx1 = sx0 + 1;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    unsigned r = 0, g = 0, b = 0, n = 0;
    for (int sy = sy0; sy < sy1; sy++) {
        const unsigned int *srow = im.px + (size_t)sy * im.w;
        for (int sx = sx0; sx < sx1; sx++) {
            unsigned c = srow[sx];
            r += (c >> 16) & 0xFF;
            g += (c >> 8) & 0xFF;
            b += c & 0xFF;
            n++;
        }
    }
    if (!n) return 0;
    return ((r / n) << 16) | ((g / n) << 8) | (b / n);
}

static void cache_build(int vw, int vh, int z) {
    if (fc_valid && fc_w == vw && fc_h == vh &&
        fc_z == z && fc_px == panx && fc_py == pany) return;

    if (!fc || fc_w != vw || fc_h != vh) {
        unsigned int *n = (unsigned int *)realloc(fc, (size_t)vw * vh * 4);
        if (!n) { fc_valid = 0; return; }
        fc = n;
        fc_w = vw;
        fc_h = vh;
    }
    fc_z = z; fc_px = panx; fc_py = pany;
    fc_valid = 1;

    int dw = im.w * z / 100, dh = im.h * z / 100;
    int ox = dw < vw ? (vw - dw) / 2 : -panx;
    int oy = dh < vh ? (vh - dh) / 2 : -pany;
    int shrink = z < 100;

    /* Hoisted: the mix is the same for every pixel, and this loop runs over
     * the whole viewport. */
    const unsigned ca = back_a(), cb = back_b();

    for (int y = 0; y < vh; y++) {
        unsigned int *row = fc + (size_t)y * vw;
        int dy = y - oy;
        int sy0 = -1, sy1 = -1;
        if (dy >= 0 && dy < dh) {
            sy0 = dy * 100 / z;
            sy1 = shrink ? (dy + 1) * 100 / z : sy0 + 1;
        }
        for (int x = 0; x < vw; x++) {
            int dx = x - ox;
            if (sy0 < 0 || dx < 0 || dx >= dw) {
                /* Behind the picture: a checkerboard, so the edges of a light
                 * image stay visible against the background. */
                row[x] = (((x >> 4) + (y >> 4)) & 1) ? ca : cb;
                continue;
            }
            int sx0 = dx * 100 / z;
            int sx1 = shrink ? (dx + 1) * 100 / z : sx0 + 1;
            row[x] = sample_box(sx0, sx1, sy0, sy1);
        }
    }
}

static void draw_image(gui_t *g, int z) {
    int vx, vy, vw, vh;
    view_rect(g, &vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    if (!im.px) { gui_fill(g, vx, vy, vw, vh, back_b()); return; }

    cache_build(vw, vh, z);
    if (!fc_valid) { gui_fill(g, vx, vy, vw, vh, back_b()); return; }

    for (int y = 0; y < vh; y++) {
        if (vy + y >= g->h) break;
        memcpy(g->px + (size_t)(vy + y) * g->w + vx,
               fc + (size_t)y * vw,
               (size_t)vw * 4);
    }
}

/* --- text painting -------------------------------------------------------- */

static void draw_text_view(gui_t *g) {
    int vx, vy, vw, vh;
    view_rect(g, &vx, &vy, &vw, &vh);
    gui_fill(g, vx, vy, vw, vh, GC_PANEL);

    int lh = g->fh + 3;
    int rows = vh / lh;
    long i = 0;
    int line = 0;

    while (line < text_top && i < text_len) {
        if (text[i] == '\n') line++;
        i++;
    }
    for (int r = 0; r < rows && i < text_len; r++) {
        int y = vy + r * lh, col = 0;
        while (i < text_len && text[i] != '\n') {
            unsigned char c = (unsigned char)text[i];
            if (c == '\t') col = (col + 4) & ~3;
            else {
                if (c < 32 || c > 126) c = '.';
                if (col >= text_left) {
                    int x = vx + 8 + (col - text_left) * g->fw;
                    if (x < vx + vw) gui_glyph(g, x, y + 1, (char)c, GC_TEXT, 1);
                }
                col++;
            }
            i++;
        }
        i++;
    }
}

/* --- chrome --------------------------------------------------------------- */

static const char *btn_label[] = { "Prev", "Next", "-", "+", "Fit", "1:1" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = (i == 2 || i == 3) ? g->fw * 4 : g->fw * 7;
    int cursor = 8;
    for (int k = 0; k < i; k++) cursor += ((k == 2 || k == 3) ? g->fw * 4 : g->fw * 7) + 5;
    *x = cursor; *y = 6; *w = bw; *h = TOOL_H - 12;
}

static void draw(gui_t *g) {
    int z = (fit && !is_text) ? fit_zoom(g) : zoom;

    if (is_text) draw_text_view(g);
    else if (im.px) draw_image(g, z);
    else {
        gui_fill(g, 0, TOOL_H, g->w, g->h - TOOL_H - STAT_H, back_b());
        gui_text(g, 20, TOOL_H + 20, "Nothing loaded.", GC_DIM);
    }

    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if (is_text && i >= 2) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label[i], st);
    }
    {
        int x, y, w, h;
        btn_rect(g, NBTN - 1, &x, &y, &w, &h);
        int tx = x + w + 14;
        char t[UPATH_MAX];
        snprintf(t, sizeof t, "%s", path);
        gui_text_clip(g, tx, (TOOL_H - g->fh) / 2, t, GC_DIM, g->w - tx - 10);
    }

    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    int sty = g->h - STAT_H + (STAT_H - g->fh) / 2;
    gui_text_clip(g, 10, sty, status, GC_TEXT, g->w - 200);
    if (!is_text && im.px) {
        char zt[48];
        snprintf(zt, sizeof zt, "%d%%%s", z, fit ? " (fit)" : "");
        gui_text(g, g->w - 10 - gui_tw(g, zt, 1), sty, zt, GC_DIM);
    }
}

/* --- actions -------------------------------------------------------------- */

static void zoom_by(gui_t *g, int dir) {
    if (is_text) return;
    if (fit) { zoom = fit_zoom(g); fit = 0; }
    if (dir > 0) zoom = zoom < 100 ? zoom * 3 / 2 + 1 : zoom + 50;
    else zoom = zoom > 100 ? zoom - 50 : zoom * 2 / 3;
    if (zoom < 5) zoom = 5;
    if (zoom > 1600) zoom = 1600;
    clamp_pan(g, zoom);
}

static void do_button(gui_t *g, int i) {
    switch (i) {
        case 0: step(-1); break;
        case 1: step(+1); break;
        case 2: zoom_by(g, -1); break;
        case 3: zoom_by(g, +1); break;
        case 4: fit = 1; panx = pany = 0; break;
        case 5: fit = 0; zoom = 100; break;
        default: break;
    }
}

int main(int argc, char **argv) {
    gui_t g;
    if (!gui_open(&g)) return 1;

    if (argc > 1) {
        load(argv[1]);
        scan_dir();
    } else {
        /* No argument: open the pictures folder, so launching this from the
         * start menu lands somewhere useful instead of on a usage line. */
        snprintf(path, sizeof path, "/pics/x");
        scan_dir();
        if (nsib > 0) {
            sibidx = 0;
            char first[UPATH_MAX];
            upath_resolve("/pics", siblings[0], first);
            load(first);
        } else {
            snprintf(status, sizeof status, "usage: view <file>");
        }
    }

    int running = 1, dirty = 1, panning = 0, lastx = 0, lasty = 0;
    int hover_btn = -2;          /* which toolbar button the pointer is over */

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) {
                    /* The cache has the old backdrop painted into it, and a
                     * theme change arrives as one of these. */
                    cache_drop();
                    gui_sync(&g);
                    continue;
                }
                switch (k->code) {
                    case XKEY_ESC: running = 0; break;
                    case XKEY_LEFT:
                        if (is_text) { text_left -= 8; if (text_left < 0) text_left = 0; }
                        else step(-1);
                        break;
                    case XKEY_RIGHT:
                        if (is_text) text_left += 8;
                        else step(+1);
                        break;
                    case XKEY_UP:
                        if (is_text) { if (text_top) text_top--; }
                        else { pany -= 40; clamp_pan(&g, fit ? fit_zoom(&g) : zoom); }
                        break;
                    case XKEY_DOWN:
                        if (is_text) { if (text_top < text_lines - 1) text_top++; }
                        else { pany += 40; clamp_pan(&g, fit ? fit_zoom(&g) : zoom); }
                        break;
                    case XKEY_PGUP:
                        if (is_text) { text_top -= 20; if (text_top < 0) text_top = 0; }
                        else { pany -= 200; clamp_pan(&g, fit ? fit_zoom(&g) : zoom); }
                        break;
                    case XKEY_PGDN:
                        if (is_text) { text_top += 20;
                                       if (text_top > text_lines - 1) text_top = text_lines - 1; }
                        else { pany += 200; clamp_pan(&g, fit ? fit_zoom(&g) : zoom); }
                        break;
                    case XKEY_HOME: text_top = 0; panx = pany = 0; break;
                    case XKEY_CHAR:
                        if (k->ascii == '+' || k->ascii == '=') zoom_by(&g, +1);
                        else if (k->ascii == '-') zoom_by(&g, -1);
                        else if (k->ascii == 'f') { fit = 1; panx = pany = 0; }
                        else if (k->ascii == '1') { fit = 0; zoom = 100; }
                        else if (k->ascii == 'q') running = 0;
                        break;
                    default: break;
                }
                continue;
            }

            mouse_event_t *m = &e.m;

            /* Only repaint for motion when the hovered button changed. */
            {
                int hb = -1;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) hb = i;
                }
                if (hb != hover_btn) { hover_btn = hb; dirty = 1; }
            }
            if (gui_acts(&e) || panning) dirty = 1;

            if (m->pressed & MB_LEFT) {
                int hit = 0;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) { do_button(&g, i); hit = 1; }
                }
                if (!hit && !is_text) {
                    panning = 1;
                    lastx = m->x;
                    lasty = m->y;
                }
            }
            if (m->released & MB_LEFT) panning = 0;
            if (panning && (m->buttons & MB_LEFT)) {
                if (fit) { zoom = fit_zoom(&g); fit = 0; }
                panx -= m->x - lastx;
                pany -= m->y - lasty;
                lastx = m->x;
                lasty = m->y;
                clamp_pan(&g, zoom);
            }
            if (m->wheel) {
                if (is_text) {
                    text_top -= m->wheel * 3;
                    if (text_top < 0) text_top = 0;
                    if (text_top > text_lines - 1) text_top = text_lines - 1;
                } else {
                    zoom_by(&g, m->wheel > 0 ? +1 : -1);
                }
            }
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g);
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;          /* the window really is gone */
            }
        }
        sleep_ms(16);
    }

    gui_close(&g);
    return 0;
}
