/* note -- the text editor (Блокнот).
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
 * gap buffer to maintain.
 *
 * Lines are UTF-8. Positions in them are bytes; what the screen and the user
 * count -- columns, the caret moving left -- are characters: a Russian letter
 * is two bytes, one cell, one press of an arrow.
 *
 * Files are opened and saved through the file manager (fdialog.h), and cut
 * and paste go through the clipboard every program shares. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui.h"
#include "fdialog.h"

#define MAX_LINES 20000
#define TAB_WIDTH 4
#define PATHMAX 1024

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

static char  *ln[MAX_LINES];
static int    llen[MAX_LINES], lcap[MAX_LINES];
static int    nlines = 1;

static char   path[PATHMAX];        /* "" while it has never been saved */
static int    modified;
static char   status[300];

static int cx, cy;          /* caret: byte in the line, line     */
static int ax = -1, ay;     /* selection anchor, ax<0 = none     */
static int topline, leftcol;    /* leftcol counts characters     */
static int want_col;        /* remembered column for up/down     */

/* Dialog modes, same single-loop approach as the file manager. */
#define M_NONE 0
#define M_FIND 2
static int        mode;
static char       edit_buf[256];
static gui_edit_t editor = { edit_buf, 256, 0, 0 };
static char       findstr[256];

/* --- characters in a line ------------------------------------------------- */

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int ucol(int y, int b) {          /* characters before byte b */
    int n = 0;
    for (int i = 0; i < b && i < llen[y]; i++) if (!is_cont(ln[y][i])) n++;
    return n;
}
static int ubyte(int y, int col) {       /* the byte where character col starts */
    int b = 0;
    while (b < llen[y] && col > 0) {
        b++;
        while (b < llen[y] && is_cont(ln[y][b])) b++;
        col--;
    }
    return b;
}
static int uback(int y, int b) {
    if (b <= 0) return 0;
    b--;
    while (b > 0 && is_cont(ln[y][b])) b--;
    return b;
}
static int ufwd(int y, int b) {
    if (b >= llen[y]) return llen[y];
    b++;
    while (b < llen[y] && is_cont(ln[y][b])) b++;
    return b;
}

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

static const char *base_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void load_file(const char *p) {
    buf_reset();
    snprintf(path, sizeof path, "%s", p);
    modified = 0;

    long fd = xyuos_open(p);
    if (fd < 0) {
        snprintf(status, sizeof status, T("Новый файл: %s", "New file: %s"), base_of(p));
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
    snprintf(status, sizeof status, T("Открыт %s — %lu байт, строк: %d", "Opened %s -- %lu bytes, %d lines"),
             base_of(p), total, nlines);
}

static int save_to(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(status, sizeof status, T("Не удалось записать %s", "Cannot write %s"), p); return 0; }
    for (int i = 0; i < nlines; i++) {
        if (llen[i]) write(fd, ln[i], (unsigned long)llen[i]);
        if (i + 1 < nlines) write(fd, "\n", 1);
    }
    close(fd);
    modified = 0;
    snprintf(status, sizeof status, T("Сохранено: %s", "Saved: %s"), base_of(p));
    return 1;
}

static void dir_of(const char *p, char *out, int max) {
    snprintf(out, (size_t)max, "%s", p);
    char *s = strrchr(out, '/');
    if (s && s != out) *s = 0;
    else if (s) s[1] = 0;
}

static void save_as(void) {
    char dir[PATHMAX], out[PATHMAX];
    if (path[0]) dir_of(path, dir, sizeof dir); else dir[0] = 0;
    const char *name = path[0] ? base_of(path) : T("Без имени.txt", "Untitled.txt");
    if (file_dialog(FD_SAVE, T("Сохранить как", "Save as"), dir[0] ? dir : 0, name, "txt",
                    T("Текстовые документы", "Text documents"), out, sizeof out)) {
        if (save_to(out)) snprintf(path, sizeof path, "%s", out);
    }
}

static void save(void) {
    if (path[0]) save_to(path);
    else save_as();
}

static void open_dialog(void) {
    char dir[PATHMAX], out[PATHMAX];
    if (path[0]) dir_of(path, dir, sizeof dir); else dir[0] = 0;
    if (file_dialog(FD_OPEN, T("Открыть", "Open"), dir[0] ? dir : 0, 0, "", 0, out, sizeof out))
        load_file(out);
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

/* The selection, onto the clipboard every program reads. */
static void sel_copy(void) {
    if (!sel_active()) return;
    int sy, sx, ey, ex;
    sel_range(&sy, &sx, &ey, &ex);
    int need = 1;
    for (int i = sy; i <= ey; i++) {
        int a = (i == sy) ? sx : 0, b = (i == ey) ? ex : llen[i];
        need += b - a + 1;
    }
    char *c = (char *)malloc((size_t)need);
    if (!c) return;
    int n = 0;
    for (int i = sy; i <= ey; i++) {
        int a = (i == sy) ? sx : 0, b = (i == ey) ? ex : llen[i];
        for (int k = a; k < b; k++) c[n++] = ln[i][k];
        if (i != ey) c[n++] = '\n';
    }
    clip_set(CLIP_TEXT, c, (unsigned)n);
    free(c);
    snprintf(status, sizeof status, T("Скопировано символов: %d", "Copied %d characters"), n);
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
    for (int i = 0; i < n; i++) if (s[i] != '\r') insert_char(s[i]);
}

static void backspace(void) {
    if (sel_active()) { sel_delete(); return; }
    if (cx > 0) {
        int from = uback(cy, cx);
        memmove(ln[cy] + from, ln[cy] + cx, (size_t)(llen[cy] - cx + 1));
        llen[cy] -= cx - from;
        cx = from;
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
        int to = ufwd(cy, cx);
        memmove(ln[cy] + cx, ln[cy] + to, (size_t)(llen[cy] - to + 1));
        llen[cy] -= to - cx;
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

static void do_paste(void) {
    int type;
    int n = clip_get(0, 0, &type);
    if (n <= 0) return;
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) return;
    clip_get(b, (unsigned)n, &type);
    b[n] = 0;
    const char *s = b;
    /* Files copied in the file manager paste as their paths. */
    if (type == CLIP_FILES) {
        const char *nl = strchr(b, '\n');
        s = nl ? nl + 1 : b + n;
        n = (int)strlen(s);
        while (n && s[n - 1] == '\n') n--;
    }
    insert_text(s, n);
    free(b);
    snprintf(status, sizeof status, "%s", T("Вставлено", "Pasted"));
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
                    snprintf(status, sizeof status, T("Найдено в строке %d", "Found on line %d"), y + 1);
                    return;
                }
            }
        }
    }
    snprintf(status, sizeof status, T("«%s» не найдено", "\"%s\" not found"), findstr);
}

/* --- layout --------------------------------------------------------------- */

#define TOOL_H 42
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
    int col = ucol(cy, cx);
    if (col < leftcol) leftcol = col;
    if (col >= leftcol + vc) leftcol = col - vc + 1;
    if (leftcol < 0) leftcol = 0;
}

/* --- drawing -------------------------------------------------------------- */

#define NBTN 7
static const char *btn_label(int i) {
    switch (i) {
    case 0: return T("Открыть", "Open");
    case 1: return T("Сохранить", "Save");
    case 2: return T("Сохранить как", "Save as");
    case 3: return T("Найти", "Find");
    case 4: return T("Вырезать", "Cut");
    case 5: return T("Копировать", "Copy");
    case 6: return T("Вставить", "Paste");
    }
    return "";
}

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int at = 8;
    for (int k = 0; k < i; k++) at += (gui_len(btn_label(k)) + 2) * g->fw + 5;
    *w = (gui_len(btn_label(i)) + 2) * g->fw;
    *h = TOOL_H - 12; *x = at; *y = 6;
}

static int clip_has(void) { int t; clip_seq(&t); return t != CLIP_NONE; }

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
            int a = (l == sy) ? ucol(l, sx) : 0;
            int b = (l == ey) ? ucol(l, ex) : ucol(l, llen[l]) + 1;
            int px0 = tx + 4 + (a - leftcol) * g->fw;
            int pw = (b - a) * g->fw;
            if (px0 < tx) { pw -= tx - px0; px0 = tx; }
            if (pw > 0) gui_fill(g, px0, y, pw, lh, GC_SEL);
        }

        int b = ubyte(l, leftcol);
        for (int c = 0; c < vc && b < llen[l]; c++) {
            int cp;
            b += gui_utf8(ln[l] + b, &cp);
            if (cp == '\t') continue;
            gui_glyph(g, tx + 4 + c * g->fw, y + 2, cp, GC_TEXT, 1);
        }
    }

    if (blink && cy >= topline && cy < topline + vl) {
        int y = ty + (cy - topline) * lh;
        int x = tx + 4 + (ucol(cy, cx) - leftcol) * g->fw;
        if (x >= tx && x < tx + tw) gui_fill(g, x, y + 1, 2, lh - 2, GC_TEXT);
    }

    /* toolbar */
    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    int bx_end = 8;
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if ((i == 4 || i == 5) && !have_sel) st = GB_OFF;
        if (i == 6 && !clip_has()) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label(i), st);
        bx_end = x + w;
    }
    {
        char t[PATHMAX + 8];
        snprintf(t, sizeof t, "%s%s", modified ? "* " : "", path[0] ? path : T("Без имени", "Untitled"));
        int x = bx_end + 14;
        gui_text_clip(g, x, (TOOL_H - g->fh) / 2, t, GC_DIM, g->w - x - 10);
    }

    /* status bar */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    int sty = g->h - STAT_H + (STAT_H - g->fh) / 2;
    gui_text_clip(g, 10, sty, status, GC_TEXT, g->w - 260);
    {
        char pos[64];
        snprintf(pos, sizeof pos, T("Стр %d, кол %d", "Ln %d, Col %d"), cy + 1, ucol(cy, cx) + 1);
        gui_text(g, g->w - 10 - gui_tw(g, pos, 1), sty, pos, GC_DIM);
    }

    if (mode != M_NONE) {
        gui_tint(g, 0, 0, g->w, g->h, 0x000000, 90);
        int dw = g->fw * 44, dh = 120;
        if (dw > g->w - 40) dw = g->w - 40;
        int dx = (g->w - dw) / 2, dy = (g->h - dh) / 2;
        gui_panel(g, dx, dy, dw, dh, GC_WIN, GC_ACCENT);
        gui_vgrad(g, dx + 1, dy + 1, dw - 2, 30, GC_ACCENT, GC_ACCENT2);
        gui_text(g, dx + 12, dy + 8, T("Найти", "Find"), 0xFFFFFF);
        gui_edit_draw(g, &editor, dx + 14, dy + 44, dw - 28, g->fh + 14, 1, blink,
                      T("что искать", "text to find"));
        int bw = g->fw * 10, bh = g->fh + 12, by = dy + dh - bh - 12;
        gui_button(g, dx + dw - 2 * bw - 22, by, bw, bh, T("Найти", "Find"),
                   gui_in(g->mx, g->my, dx + dw - 2 * bw - 22, by, bw, bh) ? GB_HOVER : GB_NORMAL);
        gui_button(g, dx + dw - bw - 12, by, bw, bh, T("Отмена", "Cancel"),
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
    cy = l;
    cx = ubyte(l, c);
}

/* --- actions -------------------------------------------------------------- */

static void do_button(int i) {
    switch (i) {
        case 0: open_dialog(); break;
        case 1: save(); break;
        case 2: save_as(); break;
        case 3: gui_edit_set(&editor, findstr); mode = M_FIND; break;
        case 4: sel_copy(); sel_delete(); break;
        case 5: sel_copy(); break;
        case 6: do_paste(); break;
        default: break;
    }
}

int main(int argc, char **argv) {
    gui_t g;
    en = ui_lang() == 1;
    if (!gui_open(&g)) return 1;

    line_room(0, 0);
    ln[0][0] = 0;
    if (argc > 1) load_file(argv[1]);
    else snprintf(status, sizeof status, "%s", T("Новый документ", "New document"));

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
                        snprintf(findstr, sizeof findstr, "%s", editor.buf);
                        find_next();
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
                    switch (k->ascii | 0x20) {
                        case 's': if (shift) save_as(); else save(); continue;
                        case 'o': open_dialog(); continue;
                        case 'c': sel_copy(); continue;
                        case 'x': sel_copy(); sel_delete(); continue;
                        case 'v': do_paste(); continue;
                        case 'f': gui_edit_set(&editor, findstr); mode = M_FIND; continue;
                        case 'a': ax = 0; ay = 0; cy = nlines - 1; cx = llen[cy]; continue;
                        case 'g': find_next(); continue;
                        case 'n': buf_reset(); path[0] = 0; modified = 0;
                                  snprintf(status, sizeof status, "%s", T("Новый документ", "New document"));
                                  continue;
                        default: continue;
                    }
                }

                switch (k->code) {
                    case XKEY_LEFT:
                        if (cx > 0) cx = uback(cy, cx);
                        else if (cy > 0) { cy--; cx = llen[cy]; }
                        want_col = ucol(cy, cx); break;
                    case XKEY_RIGHT:
                        if (cx < llen[cy]) cx = ufwd(cy, cx);
                        else if (cy + 1 < nlines) { cy++; cx = 0; }
                        want_col = ucol(cy, cx); break;
                    case XKEY_UP:
                        if (cy > 0) { cy--; cx = ubyte(cy, want_col); }
                        break;
                    case XKEY_DOWN:
                        if (cy + 1 < nlines) { cy++; cx = ubyte(cy, want_col); }
                        break;
                    case XKEY_HOME: cx = 0; want_col = 0; break;
                    case XKEY_END:  cx = llen[cy]; want_col = ucol(cy, cx); break;
                    case XKEY_PGUP:
                        cy -= vis_lines(&g); if (cy < 0) cy = 0;
                        cx = ubyte(cy, want_col);
                        break;
                    case XKEY_PGDN:
                        cy += vis_lines(&g); if (cy >= nlines) cy = nlines - 1;
                        cx = ubyte(cy, want_col);
                        break;
                    case XKEY_ENTER: insert_char('\n'); want_col = 0; break;
                    case XKEY_BKSP:  backspace(); want_col = ucol(cy, cx); break;
                    case XKEY_DEL:   del_forward(); break;
                    case XKEY_ESC:   ax = -1; break;
                    case XKEY_CHAR:
                        if (k->ascii == '\t') {
                            for (int i = 0; i < TAB_WIDTH; i++) insert_char(' ');
                        } else if ((unsigned char)k->ascii >= 32 && (unsigned char)k->ascii != 127) {
                            /* ASCII, or a byte of a UTF-8 letter */
                            insert_char(k->ascii);
                        }
                        want_col = ucol(cy, cx);
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
                int bw = g.fw * 10, bh = g.fh + 12, by = dy + dh - bh - 12;
                if (gui_clicked(&e, dx + dw - 2 * bw - 22, by, bw, bh)) {
                    snprintf(findstr, sizeof findstr, "%s", editor.buf);
                    find_next();
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
                    want_col = ucol(cy, cx);
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
