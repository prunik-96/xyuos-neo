/* devmgr -- the device manager.
 *
 * What is in this machine, which driver claimed it, and whether that driver is
 * actually running. Grouped by category and collapsible, because the answer to
 * "is my network card recognised" should not require reading past eleven PCI
 * bridges.
 *
 * It asks the kernel once and draws the answer. Nothing here probes hardware:
 * every fact shown was established by a driver at boot. A device manager that
 * went poking at the bus to fill a window would be a device manager that could
 * hang the machine by being opened.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"
#include "gui.h"

#define MAXDEV  96
#define TOOL_H  40
#define STAT_H  26
#define DET_H   140      /* four lines of detail plus its rule */
#define SBW     14

static struct si_dev devs[MAXDEV];
static int ndev;

static const char *cat_name[DEVC_COUNT] = {
    "Processors",
    "Memory",
    "Display adapters",
    "Storage",
    "Network adapters",
    "Sound",
    "USB controllers",
    "Keyboards and mice",
    "System bridges",
    "Other devices",
};

static int collapsed[DEVC_COUNT];

/* The list is a tree flattened for drawing: a category heading, then its
 * devices, then the next heading. One array so hit-testing and scrolling do
 * not each need their own idea of what row means what. */
struct row { int cat; int dev; };   /* dev < 0 for a heading */
static struct row rows[MAXDEV + DEVC_COUNT];
static int nrows;

static int sel = -1, top, hover = -1;
static char status[160];

static void rebuild_rows(void) {
    int keep = (sel >= 0 && sel < nrows) ? rows[sel].dev : -1;
    nrows = 0;
    for (int c = 0; c < DEVC_COUNT; c++) {
        int count = 0;
        for (int i = 0; i < ndev; i++) if (devs[i].cat == c) count++;
        if (!count) continue;                    /* an empty group is noise */
        rows[nrows].cat = c; rows[nrows].dev = -1; nrows++;
        if (collapsed[c]) continue;
        for (int i = 0; i < ndev; i++)
            if (devs[i].cat == c) { rows[nrows].cat = c; rows[nrows].dev = i; nrows++; }
    }
    sel = -1;
    if (keep >= 0)
        for (int i = 0; i < nrows; i++) if (rows[i].dev == keep) sel = i;
}

static void refresh(void) {
    long n = xyuos_sysinfo(SI_DEVICES, devs, sizeof devs);
    ndev = (n > 0) ? (int)(n / (long)sizeof(struct si_dev)) : 0;
    rebuild_rows();
    snprintf(status, sizeof status, "%d devices", ndev);
}

/* --- layout --------------------------------------------------------------- */

static int row_h(gui_t *g) { return g->fh + 10; }

static void list_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = TOOL_H;
    *w = g->w;
    *h = g->h - TOOL_H - STAT_H - DET_H;
    if (*h < 0) *h = 0;
}

static int visible_rows(gui_t *g) {
    int x, y, w, h;
    list_rect(g, &x, &y, &w, &h);
    int r = h / row_h(g);
    return r < 1 ? 1 : r;
}

static const char *btn_label[] = { "Refresh", "Expand all", "Collapse all" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 13, gap = 6;
    *w = bw; *h = TOOL_H - 12; *x = 8 + i * (bw + gap); *y = 6;
}

/* --- drawing -------------------------------------------------------------- */

/* A triangle, pointing right when the group is shut and down when it is open.
 * Drawn rather than stored: at this size a glyph would be a bitmap nobody can
 * edit, and the shape is four lines of arithmetic. */
static void draw_twisty(gui_t *g, int x, int y, int open, unsigned c) {
    int s = g->fh / 2;
    if (s < 4) s = 4;
    for (int i = 0; i < s; i++) {
        if (open) gui_fill(g, x + i, y + s / 2 + i, (s - i) * 2, 1, c);
        else      gui_fill(g, x + s / 2 + i, y + i, 1, (s - i) * 2, c);
    }
}

static void draw_detail(gui_t *g) {
    int y0 = g->h - STAT_H - DET_H;
    gui_fill(g, 0, y0, g->w, DET_H, GC_WIN);
    gui_fill(g, 0, y0, g->w, 1, GC_EDGE);

    int d = (sel >= 0 && sel < nrows) ? rows[sel].dev : -1;
    int ty = y0 + 8, lh = g->fh + 4;
    if (d < 0) {
        gui_text(g, 12, ty, "Select a device to see what claimed it.", GC_DIM);
        return;
    }
    struct si_dev *v = &devs[d];
    char t[160];

    gui_text_clip(g, 12, ty, v->name, GC_TEXT, g->w - 24);
    ty += lh;

    snprintf(t, sizeof t, "Category:  %s", cat_name[v->cat]);
    gui_text_clip(g, 12, ty, t, GC_DIM, g->w - 24);
    ty += lh;

    if (v->driver[0]) snprintf(t, sizeof t, "Driver:  %s   -   %s",
                               v->driver, v->status[0] ? v->status : "?");
    else              snprintf(t, sizeof t, "Driver:  none   -   %s",
                               v->status[0] ? v->status : "not claimed");
    gui_text_clip(g, 12, ty, t, v->driver[0] ? GC_TEXT : GC_WARN, g->w - 24);
    ty += lh;

    if (v->bus != 0xFF) {
        snprintf(t, sizeof t, "PCI %02x:%02x.%x   vendor %04x  device %04x",
                 v->bus, v->slot, v->func, v->vendor, v->device);
        gui_text_clip(g, 12, ty, t, GC_DIM, g->w - 24);
    } else {
        gui_text(g, 12, ty, "Not on the PCI bus", GC_DIM);
    }
}

static void draw(gui_t *g) {
    gui_clear(g, GC_WIN);

    /* toolbar */
    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        gui_button(g, x, y, w, h, btn_label[i],
                   gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL);
    }

    /* the tree */
    int lx, ly, lw, lh;
    list_rect(g, &lx, &ly, &lw, &lh);
    gui_fill(g, lx, ly, lw, lh, GC_PANEL);
    int rh = row_h(g), vis = visible_rows(g);
    int need_sb = nrows > vis;
    int inner = lw - (need_sb ? SBW : 0);

    for (int i = 0; i < vis && top + i < nrows; i++) {
        int idx = top + i, y = ly + i * rh;
        struct row *r = &rows[idx];
        int ty = y + (rh - g->fh) / 2;

        if (r->dev < 0) {
            int count = 0;
            for (int k = 0; k < ndev; k++) if (devs[k].cat == r->cat) count++;
            gui_fill(g, lx, y, inner, rh, GC_BAR);
            draw_twisty(g, lx + 10, y + (rh - g->fh / 2) / 2, !collapsed[r->cat], GC_DIM);
            char t[80];
            snprintf(t, sizeof t, "%s  (%d)", cat_name[r->cat], count);
            gui_text_clip(g, lx + 10 + g->fh, ty, t, GC_TEXT, inner - 40);
            continue;
        }

        struct si_dev *v = &devs[r->dev];
        unsigned bg = (idx & 1) ? GC_ALT : GC_PANEL;
        if (idx == hover) bg = GC_HOT;
        if (idx == sel)   bg = GC_SEL;
        gui_fill(g, lx, y, inner, rh, bg);
        if (idx == sel) gui_fill(g, lx, y, 3, rh, GC_ACCENT);

        int namex = lx + 12 + g->fh;
        int statx = inner * 60 / 100;
        gui_text_clip(g, namex, ty, v->name, GC_TEXT, statx - namex - 12);

        /* A device with no driver is the whole reason to open this window, so
         * it is the one thing here that is coloured. */
        const char *st = v->status[0] ? v->status : "unknown";
        unsigned c = v->driver[0] ? GC_DIM : GC_WARN;
        gui_text_clip(g, statx, ty, st, c, inner - statx - 8);
    }
    if (need_sb) gui_scrollbar(g, lx + lw - SBW, ly, SBW, lh, top, vis, nrows);

    draw_detail(g);

    /* status bar */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    gui_text_clip(g, 10, g->h - STAT_H + (STAT_H - g->fh) / 2, status,
                  GC_TEXT, g->w - 20);
}

/* --- actions -------------------------------------------------------------- */

static void act(int which) {
    switch (which) {
        case 0: refresh(); break;
        case 1: for (int c = 0; c < DEVC_COUNT; c++) collapsed[c] = 0; rebuild_rows(); break;
        case 2: for (int c = 0; c < DEVC_COUNT; c++) collapsed[c] = 1; rebuild_rows(); break;
    }
}

static void toggle(int idx) {
    if (idx < 0 || idx >= nrows) return;
    int c = rows[idx].cat;
    collapsed[c] = !collapsed[c];
    rebuild_rows();
    if (top > nrows) top = 0;
}

int main(void) {
    gui_t g;
    if (!gui_open(&g)) return 1;
    refresh();

    int running = 1, dirty = 1, dragging_sb = 0;

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }
                switch (k->code) {
                    case XKEY_UP:    if (sel > 0) sel--; break;
                    case XKEY_DOWN:  if (sel < nrows - 1) sel++; break;
                    case XKEY_HOME:  sel = nrows ? 0 : -1; break;
                    case XKEY_END:   sel = nrows - 1; break;
                    case XKEY_LEFT:  if (sel >= 0 && rows[sel].dev < 0) toggle(sel); break;
                    case XKEY_RIGHT: if (sel >= 0 && rows[sel].dev < 0) toggle(sel); break;
                    case XKEY_ENTER: if (sel >= 0 && rows[sel].dev < 0) toggle(sel); break;
                    case XKEY_ESC:   running = 0; break;
                    default:
                        if (k->code == XKEY_F(5)) refresh();
                        else if (k->code == XKEY_CHAR && k->ascii == 'q') running = 0;
                        break;
                }
                int vis = visible_rows(&g);
                if (sel >= 0) {
                    if (sel < top) top = sel;
                    if (sel >= top + vis) top = sel - vis + 1;
                }
                continue;
            }

            mouse_event_t *m = &e.m;
            int lx, ly, lw, lh;
            list_rect(&g, &lx, &ly, &lw, &lh);
            int rh = row_h(&g), vis = visible_rows(&g);

            int was_hover = hover;
            hover = -1;
            if (gui_in(m->x, m->y, lx, ly, lw - (nrows > vis ? SBW : 0), lh)) {
                int i = top + (m->y - ly) / rh;
                if (i >= 0 && i < nrows) hover = i;
            }
            if (hover != was_hover || gui_acts(&e) || dragging_sb) dirty = 1;
            if (m->released & MB_LEFT) dragging_sb = 0;
            if (m->wheel) {
                top -= m->wheel * 3;
                if (top > nrows - vis) top = nrows - vis;
                if (top < 0) top = 0;
            }
            if (dragging_sb && (m->buttons & MB_LEFT))
                top = gui_scrollbar_pick(ly, lh, m->y, vis, nrows);

            if (m->pressed & MB_LEFT) {
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) act(i);
                }
                if (nrows > vis && gui_in(m->x, m->y, lx + lw - SBW, ly, SBW, lh)) {
                    dragging_sb = 1;
                    top = gui_scrollbar_pick(ly, lh, m->y, vis, nrows);
                } else if (hover >= 0) {
                    if (rows[hover].dev < 0) toggle(hover);
                    else                     sel = hover;
                }
            }
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
        sleep_ms(30);
    }

    gui_close(&g);
    return 0;
}
