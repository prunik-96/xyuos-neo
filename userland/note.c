/* note -- the text editor.
 *
 * The system already had `edit`, which drives a character grid. This one has a
 * caret you can put somewhere with the mouse, a selection you can drag, and a
 * gutter -- none of which a cell-addressed terminal editor can do, because in
 * a terminal the smallest thing you can point at is a whole character.
 *
 * The buffer is an array of independently allocated lines rather than one flat
 * text blob. Editing a line then costs nothing but a memmove inside that line,
 * and splitting or joining lines costs a memmove of the line POINTERS -- which
 * is what makes typing feel instant in a 5000-line file without a rope or a
 * gap buffer to maintain. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui.h"
#include "upath.h"

#define MAX_LINES 20000
#define TAB_WIDTH 4

static char  *ln[MAX_LINES];
static int    llen[MAX_LINES], lcap[MAX_LINES];
static int    nlines = 1;

static char   path[UPATH_MAX];
static int    modified;
static char   status[200];

static int cx, cy;          /* caret: column, line          */
static int ax = -1, ay;     /* selection anchor, ax<0 = none */
static int topline, leftcol;
static int want_col;        /* remembered column for up/down */

static char  *clip;
static int    clip_len;

/* Dialog modes, same single-loop approach as the file manager. */
#define M_NONE 0
#define M_SAVEAS 1
#define M_FIND 2
static int        mode;
static char       edit_buf[UPATH_MAX];
static gui_edit_t editor = { edit_buf, UPATH_MAX, 0, 0 };
static char       findstr[128];

/* --- buffer primitives ---------------------------------------------------- */

static int line_room(int i, int need) {
    if (lcap[i] >= need + 1) return 1;
    int cap = lcap[i] ? lcap[i] : 32;
    while (cap < need + 1) cap *= 2;
    char *n = (char *)realloc(ln[i], (size_t)cap);
    if (!n) return 0;
    ln[i] = n;
    lcap[i] = cap;
    return 1;
}

static void buf_reset(void) {
    for (int i = 0; i < nlines; i++) { free(ln[i]); ln[i] = 0; llen[i] = lcap[i] = 0; }
    nlines = 1;
    line_room(0, 0);
    ln[0][0] = 0;
    llen[0] = 0;
    cx = cy = topline = leftcol = 0;
    ax = -1;
}

static int insert_line(int at) {
    if (nlines >= MAX_LINES) return 0;
    memmove(&ln[at + 1], &ln[at], (size_t)(nlines - at) * sizeof(char *));
    memmove(&llen[at + 1], &llen[at], (size_t)(nlines - at) * sizeof(int));
    memmove(&lcap[at + 1], &lcap[at], (size_t)(nlines - at) * sizeof(int));
    ln[at] = 0; llen[at] = lcap[at] = 0;
    nlines++;
    return line_room(at, 0);
}

static void remove_line(int at) {
    free(ln[at]);
    memmove(&ln[at], &ln[at + 1], (size_t)(nlines - at - 1) * sizeof(char *));
    memmove(&llen[at], &llen[at + 1], (size_t)(nlines - at - 1) * sizeof(int));
    memmove(&lcap[at], &lcap[at + 1], (size_t)(nlines - at - 1) * sizeof(int));
    nlines--;
    if (nlines == 0) { nlines = 1; line_room(0, 0); ln[0][0] = 0; llen[0] = 0; }
}

/* --- load / save ---------------------------------------------------------- */

static void load_file(const char *p) {
    buf_reset();
    snprintf(path, sizeof path, "%s", p);
    modified = 0;

    long fd = xyuos_open(p);
    if (fd < 0) {
        snprintf(status, sizeof status, "new file: %s", p);
        return;
    }
    char buf[4096];
    long n;
    int cur = 0;
    unsigned long total = 0;
    while ((n = xyuos_read(fd, buf, sizeof buf)) > 0) {
        total += (unsigned long)n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (cur + 1 >= MAX_LINES) { i = n; break; }
                if (!insert_line(cur + 1)) break;
                cur++;
                continue;
            }
            if (!line_room(cur, llen[cur] + 1)) break;
            ln[cur][llen[cur]++] = c;
            ln[cur][llen[cur]] = 0;
        }
    }
    xyuos_close(fd);
    snprintf(status, sizeof status, "%s -- %lu bytes, %d lines", p, total, nlines);
}

static int save_to(const char *p) {
    xyuos_create(p);                       /* truncates if it already exists */
    int fd = open(p, O_WRONLY);
    if (fd < 0) { snprintf(status, sizeof status, "cannot write %s", p); return 0; }
    for (int i = 0; i < nlines; i++) {
        if (llen[i]) write(fd, ln[i], (unsigned long)llen[i]);
        if (i + 1 < nlines) write(fd, "\n", 1);
    }
    close(fd);
    modified = 0;
    snprintf(status, sizeof status, "saved %s (%d lines)", p, nlines);
    return 1;
}

/* --- selection ------------------------------------------------------------ */

static int sel_active(void) { return ax >= 0 && (ax != cx || ay != cy); }

static void sel_range(int *sy, int *sx, int *ey, int *ex) {
    if (ay < cy || (ay == cy && ax <= cx)) { *sy = ay; *sx = ax; *ey = cy; *ex = cx; }
    else                                   { *sy = cy; *sx = cx; *ey = ay; *ex = ax; }
}

static void sel_delete(void) {
    if (!sel_active()) return;
    int sy, sx, ey, ex;
    sel_range(&sy, &sx, &ey, &ex);
    if (sy == ey) {
        memmove(ln[sy] + sx, ln[sy] + ex, (size_t)(llen[sy] - ex + 1));
        llen[sy] -= ex - sx;
    } else {
        int tail = llen[ey] - ex;
        if (line_room(sy, sx + tail)) {
            memmove(ln[sy] + sx, ln[ey] + ex, (size_t)tail);
            llen[sy] = sx + tail;
            ln[sy][llen[sy]] = 0;
        }
        for (int i = 0; i < ey - sy; i++) remove_line(sy + 1);
    }
    cy = sy; cx = sx;
    ax = -1;
    modified = 1;
}

static void sel_copy(void) {
    if (!sel_active()) return;
    int sy, sx, ey, ex;
    sel_range(&sy, &sx, &ey, &ex);
    int need = 1;
    for (int i = sy; i <= ey; i++) {
        int a = (i == sy) ? sx : 0, b = (i == ey) ? ex : llen[i];
        need += b - a + 1;
    }
    char *n = (char *)realloc(clip, (size_t)need);
    if (!n) return;
    clip = n;
    clip_len = 0;
    for (int i = sy; i <= ey; i++) {
        int a = (i == sy) ? sx : 0, b = (i == ey) ? ex : llen[i];
        for (int k = a; k < b; k++) clip[clip_len++] = ln[i][k];
        if (i != ey) clip[clip_len++] = '\n';
    }
    clip[clip_len] = 0;
    snprintf(status, sizeof status, "copied %d bytes", clip_len);
}

/* --- editing -------------------------------------------------------------- */

static void insert_char(char c) {
    if (sel_active()) sel_delete();
    if (c == '\n') {
        if (!insert_line(cy + 1)) return;
        int tail = llen[cy] - cx;
        if (line_room(cy + 1, tail)) {
            memmove(ln[cy + 1], ln[cy] + cx, (size_t)tail);
            llen[cy + 1] = tail;
            ln[cy + 1][tail] = 0;
        }
        llen[cy] = cx;
        ln[cy][cx] = 0;
        cy++; cx = 0;
    } else {
        if (!line_room(cy, llen[cy] + 1)) return;
        memmove(ln[cy] + cx + 1, ln[cy] + cx, (size_t)(llen[cy] - cx + 1));
        ln[cy][cx++] = c;
        llen[cy]++;
    }
    modified = 1;
    ax = -1;
}

static void insert_text(const char *s, int n) {
    for (int i = 0; i < n; i++) insert_char(s[i]);
}

static void backspace(void) {
    if (sel_active()) { sel_delete(); return; }
    if (cx > 0) {
        memmove(ln[cy] + cx - 1, ln[cy] + cx, (size_t)(llen[cy] - cx + 1));
        cx--; llen[cy]--;
        modified = 1;
    } else if (cy > 0) {
        int prev = cy - 1, at = llen[prev];
        if (line_room(prev, at + llen[cy])) {
            memmove(ln[prev] + at, ln[cy], (size_t)(llen[cy] + 1));
            llen[prev] = at + llen[cy];
        }
        remove_line(cy);
        cy = prev; cx = at;
        modified = 1;
    }
}

static void del_forward(void) {
    if (sel_active()) { sel_delete(); return; }
    if (cx < llen[cy]) {
        memmove(ln[cy] + cx, ln[cy] + cx + 1, (size_t)(llen[cy] - cx));
        llen[cy]--;
        modified = 1;
    } else if (cy + 1 < nlines) {
        if (line_room(cy, llen[cy] + llen[cy + 1])) {
            memmove(ln[cy] + llen[cy], ln[cy + 1], (size_t)(llen[cy + 1] + 1));
            llen[cy] += llen[cy + 1];
        }
        remove_line(cy + 1);
        modified = 1;
    }
}

/* --- find ----------------------------------------------------------------- */

static void find_next(void) {
    int n = (int)strlen(findstr);
    if (!n) return;
    for (int pass = 0; pass < 2; pass++) {
        int y = pass ? 0 : cy;
        int x = pass ? 0 : cx + 1;
        for (; y < nlines; y++) {
            for (int i = (y == cy && !pass) ? x : 0; i + n <= llen[y]; i++) {
                if (memcmp(ln[y] + i, findstr, (size_t)n) == 0) {
                    cy = y; cx = i + n;
                    ay = y; ax = i;
                    snprintf(status, sizeof status, "found at line %d", y + 1);
                    return;
                }
            }
        }
    }
    snprintf(status, sizeof status, "\"%s\" not found", findstr);
}

/* --- layout --------------------------------------------------------------- */

#define TOOL_H 40
#define STAT_H 26

static int gutter_w(gui_t *g) {
    int digits = 1, n = nlines;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 3) digits = 3;
    return (digits + 2) * g->fw;
}

static int line_h(gui_t *g) { return g->fh + 4; }

static void text_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = gutter_w(g);
    *y = TOOL_H;
    *w = g->w - *x;
    *h = g->h - TOOL_H - STAT_H;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static int vis_lines(gui_t *g) {
    int x, y, w, h;
    text_rect(g, &x, &y, &w, &h);
    int n = h / line_h(g);
    return n < 1 ? 1 : n;
}

static int vis_cols(gui_t *g) {
    int x, y, w, h;
    text_rect(g, &x, &y, &w, &h);
    int n = (w - 8) / g->fw;
    return n < 1 ? 1 : n;
}

static void caret_visible(gui_t *g) {
    int vl = vis_lines(g), vc = vis_cols(g);
    if (cy < topline) topline = cy;
    if (cy >= topline + vl) topline = cy - vl + 1;
    if (topline < 0) topline = 0;
    if (cx < leftcol) leftcol = cx;
    if (cx >= leftcol + vc) leftcol = cx - vc + 1;
    if (leftcol < 0) leftcol = 0;
}

/* --- drawing -------------------------------------------------------------- */

static const char *btn_label[] = { "Save", "Save as", "Find", "Cut", "Copy", "Paste" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 9, gap = 5;
    *w = bw; *h = TOOL_H - 12; *x = 8 + i * (bw + gap); *y = 6;
}

static void draw(gui_t *g, int blink) {
    gui_clear(g, GC_PANEL);

    int tx, ty, tw, th;
    text_rect(g, &tx, &ty, &tw, &th);
    int lh = line_h(g), vl = vis_lines(g), vc = vis_cols(g);

    /* gutter */
    gui_fill(g, 0, ty, tx, th, GC_BAR);
    gui_fill(g, tx - 1, ty, 1, th, GC_LINE);

    int sy = 0, sx = 0, ey = 0, ex = 0, have_sel = sel_active();
    if (have_sel) sel_range(&sy, &sx, &ey, &ex);

    for (int i = 0; i < vl && topline + i < nlines; i++) {
        int y = ty + i * lh, l = topline + i;

        char num[16];
        snprintf(num, sizeof num, "%d", l + 1);
        gui_text(g, tx - g->fw - gui_tw(g, num, 1), y + 2, num,
                 l == cy ? GC_ACCENT : GC_DIM);

        if (l == cy && !have_sel) gui_fill(g, tx, y, tw, lh, GC_HOT);

        if (have_sel && l >= sy && l <= ey) {
            int a = (l == sy) ? sx : 0;
            int b = (l == ey) ? ex : llen[l] + 1;
            if (b > llen[l] + 1) b = llen[l] + 1;
            int px0 = tx + 4 + (a - leftcol) * g->fw;
            int pw = (b - a) * g->fw;
            if (px0 < tx) { pw -= tx - px0; px0 = tx; }
            if (pw > 0) gui_fill(g, px0, y, pw, lh, GC_SEL);
        }

        for (int c = 0; c < vc && leftcol + c < llen[l]; c++) {
            char ch = ln[l][leftcol + c];
            if (ch == '\t') continue;
            gui_glyph(g, tx + 4 + c * g->fw, y + 2, ch, GC_TEXT, 1);
        }
    }

    if (blink && cy >= topline && cy < topline + vl) {
        int y = ty + (cy - topline) * lh;
        int x = tx + 4 + (cx - leftcol) * g->fw;
        if (x >= tx && x < tx + tw) gui_fill(g, x, y + 1, 2, lh - 2, GC_TEXT);
    }

    /* toolbar */
    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if ((i == 3 || i == 4) && !have_sel) st = GB_OFF;
        if (i == 5 && !clip_len) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label[i], st);
    }
    {
        char t[UPATH_MAX + 8];
        snprintf(t, sizeof t, "%s%s", modified ? "* " : "", path);
        int x = 8 + NBTN * (g->fw * 9 + 5) + 10;
        gui_text_clip(g, x, (TOOL_H - g->fh) / 2, t, GC_DIM, g->w - x - 10);
    }

    /* status bar */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    int sty = g->h - STAT_H + (STAT_H - g->fh) / 2;
    gui_text_clip(g, 10, sty, status, GC_TEXT, g->w - 220);
    {
        char pos[64];
        snprintf(pos, sizeof pos, "Ln %d, Col %d", cy + 1, cx + 1);
        gui_text(g, g->w - 10 - gui_tw(g, pos, 1), sty, pos, GC_DIM);
    }

    if (mode != M_NONE) {
        gui_tint(g, 0, 0, g->w, g->h, 0x000000, 90);
        int dw = g->fw * 44, dh = 120;
        if (dw > g->w - 40) dw = g->w - 40;
        int dx = (g->w - dw) / 2, dy = (g->h - dh) / 2;
        gui_panel(g, dx, dy, dw, dh, GC_WIN, GC_ACCENT);
        gui_vgrad(g, dx + 1, dy + 1, dw - 2, 30, GC_ACCENT, GC_ACCENT2);
        gui_text(g, dx + 12, dy + 8, mode == M_SAVEAS ? "Save as" : "Find", 0xFFFFFF);
        gui_edit_draw(g, &editor, dx + 14, dy + 44, dw - 28, g->fh + 14, 1, blink,
                      mode == M_SAVEAS ? "/path/to/file" : "text to find");
        int bw = g->fw * 9, bh = g->fh + 12, by = dy + dh - bh - 12;
        gui_button(g, dx + dw - 2 * bw - 22, by, bw, bh, "OK",
                   gui_in(g->mx, g->my, dx + dw - 2 * bw - 22, by, bw, bh) ? GB_HOVER : GB_NORMAL);
        gui_button(g, dx + dw - bw - 12, by, bw, bh, "Cancel",
                   gui_in(g->mx, g->my, dx + dw - bw - 12, by, bw, bh) ? GB_HOVER : GB_NORMAL);
    }
}

/* --- pointer -> caret ----------------------------------------------------- */

static void caret_from_point(gui_t *g, int px, int py) {
    int tx, ty, tw, th;
    text_rect(g, &tx, &ty, &tw, &th);
    int l = topline + (py - ty) / line_h(g);
    if (l < 0) l = 0;
    if (l >= nlines) l = nlines - 1;
    int c = leftcol + (px - tx - 4 + g->fw / 2) / g->fw;
    if (c < 0) c = 0;
    if (c > llen[l]) c = llen[l];
    cy = l; cx = c;
}

/* --- actions -------------------------------------------------------------- */

static void do_paste(void) {
    if (clip_len) { insert_text(clip, clip_len); snprintf(status, sizeof status, "pasted"); }
}

static void do_button(int i) {
    switch (i) {
        case 0: save_to(path); break;
        case 1: gui_edit_set(&editor, path); mode = M_SAVEAS; break;
        case 2: gui_edit_set(&editor, findstr); mode = M_FIND; break;
        case 3: sel_copy(); sel_delete(); break;
        case 4: sel_copy(); break;
        case 5: do_paste(); break;
        default: break;
    }
}

int main(int argc, char **argv) {
    gui_t g;
    if (!gui_open(&g)) return 1;

    line_room(0, 0);
    ln[0][0] = 0;
    if (argc > 1) load_file(argv[1]);
    else { snprintf(path, sizeof path, "/untitled.txt");
           snprintf(status, sizeof status, "new file"); }

    int running = 1, dirty = 1, selecting = 0, last_blink = -1;

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }

                if (mode != M_NONE) {
                    if (k->code == XKEY_ESC) { mode = M_NONE; continue; }
                    if (k->code == XKEY_ENTER) {
                        if (mode == M_SAVEAS && editor.len) {
                            char abs[UPATH_MAX];
                            upath_resolve("/", editor.buf, abs);
                            if (save_to(abs)) snprintf(path, sizeof path, "%s", abs);
                        } else if (mode == M_FIND) {
                            snprintf(findstr, sizeof findstr, "%s", editor.buf);
                            find_next();
                        }
                        mode = M_NONE;
                        continue;
                    }
                    gui_edit_key(&editor, k);
                    continue;
                }

                int ctrl = k->mods & XMOD_CTRL, shift = k->mods & XMOD_SHIFT;

                /* Start or clear a keyboard selection before the caret moves. */
                int is_move = (k->code == XKEY_LEFT || k->code == XKEY_RIGHT ||
                               k->code == XKEY_UP || k->code == XKEY_DOWN ||
                               k->code == XKEY_HOME || k->code == XKEY_END ||
                               k->code == XKEY_PGUP || k->code == XKEY_PGDN);
                if (is_move) {
                    if (shift) { if (ax < 0) { ax = cx; ay = cy; } }
                    else ax = -1;
                }

                if (ctrl && k->code == XKEY_CHAR) {
                    switch (k->ascii) {
                        case 's': case 19: save_to(path); continue;
                        case 'c': case  3: sel_copy(); continue;
                        case 'x': case 24: sel_copy(); sel_delete(); continue;
                        case 'v': case 22: do_paste(); continue;
                        case 'f': case  6: gui_edit_set(&editor, findstr);
                                           mode = M_FIND; continue;
                        case 'a': case  1: ax = 0; ay = 0;
                                           cy = nlines - 1; cx = llen[cy]; continue;
                        case 'g': case  7: find_next(); continue;
                        default: break;
                    }
                }

                switch (k->code) {
                    case XKEY_LEFT:
                        if (cx > 0) cx--;
                        else if (cy > 0) { cy--; cx = llen[cy]; }
                        want_col = cx; break;
                    case XKEY_RIGHT:
                        if (cx < llen[cy]) cx++;
                        else if (cy + 1 < nlines) { cy++; cx = 0; }
                        want_col = cx; break;
                    case XKEY_UP:
                        if (cy > 0) { cy--; cx = want_col > llen[cy] ? llen[cy] : want_col; }
                        break;
                    case XKEY_DOWN:
                        if (cy + 1 < nlines) { cy++; cx = want_col > llen[cy] ? llen[cy] : want_col; }
                        break;
                    case XKEY_HOME: cx = 0; want_col = 0; break;
                    case XKEY_END:  cx = llen[cy]; want_col = cx; break;
                    case XKEY_PGUP:
                        cy -= vis_lines(&g); if (cy < 0) cy = 0;
                        if (cx > llen[cy]) cx = llen[cy];
                        break;
                    case XKEY_PGDN:
                        cy += vis_lines(&g); if (cy >= nlines) cy = nlines - 1;
                        if (cx > llen[cy]) cx = llen[cy];
                        break;
                    case XKEY_ENTER: insert_char('\n'); want_col = cx; break;
                    case XKEY_BKSP:  backspace(); want_col = cx; break;
                    case XKEY_DEL:   del_forward(); break;
                    case XKEY_ESC:   ax = -1; break;
                    case XKEY_CHAR:
                        if (k->ascii == '\t') {
                            for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
                        } else if ((unsigned char)k->ascii >= 32 &&
                                   (unsigned char)k->ascii < 127) {
                            insert_char(k->ascii);
                        }
                        want_col = cx;
                        break;
                    default:
                        if (k->code == XKEY_F(3)) find_next();
                        break;
                }
                caret_visible(&g);
                continue;
            }

            /* --- pointer --- */
            mouse_event_t *m = &e.m;
            if (gui_acts(&e) || selecting) dirty = 1;
            if (mode != M_NONE) {
                int dw = g.fw * 44, dh = 120;
                if (dw > g.w - 40) dw = g.w - 40;
                int dx = (g.w - dw) / 2, dy = (g.h - dh) / 2;
                int bw = g.fw * 9, bh = g.fh + 12, by = dy + dh - bh - 12;
                if (gui_clicked(&e, dx + dw - 2 * bw - 22, by, bw, bh)) {
                    if (mode == M_SAVEAS && editor.len) {
                        char abs[UPATH_MAX];
                        upath_resolve("/", editor.buf, abs);
                        if (save_to(abs)) snprintf(path, sizeof path, "%s", abs);
                    } else if (mode == M_FIND) {
                        snprintf(findstr, sizeof findstr, "%s", editor.buf);
                        find_next();
                    }
                    mode = M_NONE;
                } else if (gui_clicked(&e, dx + dw - bw - 12, by, bw, bh)) mode = M_NONE;
                continue;
            }

            int tx, ty, tw, th;
            text_rect(&g, &tx, &ty, &tw, &th);

            if (m->wheel) {
                topline -= m->wheel * 3;
                if (topline > nlines - 1) topline = nlines - 1;
                if (topline < 0) topline = 0;
            }
            if (m->pressed & MB_LEFT) {
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) { do_button(i); goto done; }
                }
                if (gui_in(m->x, m->y, 0, ty, g.w, th)) {
                    caret_from_point(&g, m->x, m->y);
                    ax = cx; ay = cy;          /* anchor for a drag selection */
                    selecting = 1;
                    want_col = cx;
                }
            }
            if (selecting && (m->buttons & MB_LEFT) && !(m->pressed & MB_LEFT)) {
                caret_from_point(&g, m->x, m->y);
                caret_visible(&g);
            }
            if (m->released & MB_LEFT) selecting = 0;
done:;
        }

        if (dirty) {
            if (gui_sync(&g)) {
                caret_visible(&g);
                draw(&g, (int)((uptime_ms() / 500) & 1));
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;          /* the window really is gone */
            }
        }
        /* The caret blinks, so an idle editor still has to repaint -- but
         * only when the phase actually flips. Repainting every 80 ms instead
         * cost a tenth of the machine to animate one rectangle. */
        sleep_ms(60);
        int phase = (int)((uptime_ms() / 500) & 1);
        if (phase != last_blink) { last_blink = phase; dirty = 1; }
    }

    gui_close(&g);
    return 0;
}
