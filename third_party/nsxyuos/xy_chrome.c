/* The part of the browser that is not the page.
 *
 * A strip of tabs, a toolbar, an address field, a message line. None of it
 * comes from NetSurf -- a frontend is expected to provide its own, which is
 * why the same engine looks like a GTK program on one system and like this on
 * another.
 *
 * The buttons are drawn rather than lettered. The window manager's font
 * covers Latin, Latin-1, Latin Extended-A and Cyrillic; the arrows a toolbar
 * wants live at U+2190 and are not in it. Two triangles and an arc are less
 * code than teaching the font about them, and they scale with the bar.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"

#include "xy_front.h"
#include "xy_chrome.h"

/* A dark bar over a light page: the page is what should be bright, and
 * chrome that competes with it for attention is chrome done wrong. */
#define C_BAR      0x1C2026u
#define C_TABS     0x141820u
#define C_TAB_ON   0x1C2026u
#define C_TAB_OFF  0x10141Au
#define C_FIELD    0x0C1016u
#define C_TEXT     0xE6E9EDu
#define C_DIM      0x7F8894u
#define C_EDGE     0x2A3038u
#define C_ACCENT   0x4C9AFFu
#define C_HOVER    0x262C34u

struct hit xy_hits[XY_HIT_MAX];
int xy_nhits;

static void hit(int kind, int arg, int x, int y, int w, int h) {
    if (xy_nhits >= XY_HIT_MAX) return;
    xy_hits[xy_nhits++] = (struct hit){ kind, arg, x, y, w, h };
}

/* --- small drawing helpers ----------------------------------------------- */

/* A filled triangle pointing left or right: the two history buttons. */
static void arrow(int cx, int cy, int size, int dir, unsigned col) {
    for (int i = 0; i < size; i++) {
        int half = i;                       /* widens away from the point */
        int x = cx + (dir > 0 ? (size / 2 - i) : (i - size / 2));
        for (int y = cy - half; y <= cy + half; y++)
            gui_fill(&xy_gui, x, y, 1, 1, col);
    }
}

/* An open circle with a tick at one end: reload. Drawn from the circle
 * definition rather than a table, which is three lines and exact. */
static void reload_glyph(int cx, int cy, int r, unsigned col) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d = dx * dx + dy * dy;
            if (d > r * r || d < (r - 2) * (r - 2)) continue;
            if (dx > 0 && dy < 0) continue;          /* the gap */
            gui_fill(&xy_gui, cx + dx, cy + dy, 1, 1, col);
        }
    }
    /* The head of the arrow, where the gap is. */
    for (int i = 0; i < 4; i++)
        gui_fill(&xy_gui, cx + r - 3 + i, cy - r + 1, 1, i + 1, col);
}

/* A square: stop. */
static void stop_glyph(int cx, int cy, int r, unsigned col) {
    gui_fill(&xy_gui, cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2, col);
}

/* A house outline: home. */
static void home_glyph(int cx, int cy, int r, unsigned col) {
    for (int i = 0; i <= r; i++)                    /* the roof */
        gui_fill(&xy_gui, cx - i, cy - r + i, 2 * i + 1, 1, col);
    gui_fill(&xy_gui, cx - r + 2, cy, 2 * r - 3, r, col);
}

static int button(int x, int y, int w, int h, int hot, int enabled) {
    if (!enabled) return 0;
    if (hot) gui_fill(&xy_gui, x, y, w, h, C_HOVER);
    return 1;
}

static int inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* A line of chrome text, centred in a strip of height h, no wider than maxw.
 * A page's title is in whatever script the page is in, so with fonts on the
 * disk this goes through libtext like the page does; one too long ends in an
 * ellipsis rather than simply stopping. The address field is the exception:
 * it is an editor, with a cursor that moves a character cell at a time, and
 * an address is nearly always ASCII -- it keeps the cell font. */
static void chrome_text(int x, int y, int h, const char *s, unsigned col, int maxw) {
    if (!xy_text) {
        gui_text_clip(&xy_gui, x, y + (h - xy_gui.fh) / 2, s, col, maxw);
        return;
    }
    txt_style st = { TXT_SANS, 400, 0, 13 * 64 };
    int asc, desc;
    txt_metrics(&st, &asc, &desc);
    int base = y + (h + asc - desc) / 2;
    txt_target t = { xy_gui.px, xy_gui.w, x, y, x + maxw, y + h };
    if (t.x1 > xy_gui.w) t.x1 = xy_gui.w;
    if (t.y1 > xy_gui.h) t.y1 = xy_gui.h;

    size_t len = strlen(s);
    if (txt_width(&st, s, len) <= maxw) {
        txt_draw(&t, &st, x, base, col, s, len);
        return;
    }
    static const char dots[] = "\xE2\x80\xA6";          /* the ellipsis */
    int dw = txt_width(&st, dots, 3), at = 0;
    size_t keep = txt_hit(&st, s, len, maxw - dw, &at);
    if (at > maxw - dw && keep > 0)                   /* nearest may be past it */
        keep = txt_hit(&st, s, keep, at - 1, &at);
    txt_draw(&t, &st, x, base, col, s, keep);
    txt_draw(&t, &st, x + at, base, col, dots, 3);
}

/* --- the tabs ------------------------------------------------------------ */

static void draw_tabs(int mx, int my) {
    gui_fill(&xy_gui, 0, 0, xy_gui.w, XY_TABS_H, C_TABS);

    if (xy_ntabs <= 0) return;

    int room = xy_gui.w - 30;                  /* the + button at the end */
    int tw = room / (xy_ntabs > 0 ? xy_ntabs : 1);
    if (tw > 180) tw = 180;
    if (tw < 40) tw = 40;

    for (int i = 0; i < xy_ntabs; i++) {
        int x = i * tw;
        if (x + tw > room) break;
        int on = (i == xy_tab);

        gui_fill(&xy_gui, x, 0, tw - 1, XY_TABS_H, on ? C_TAB_ON : C_TAB_OFF);
        if (on)
            gui_fill(&xy_gui, x, 0, tw - 1, 2, C_ACCENT);   /* which one */

        const char *name = xy_tabs[i]->title[0] ? xy_tabs[i]->title
                                                : "loading";
        chrome_text(x + 8, 0, XY_TABS_H, name, on ? C_TEXT : C_DIM, tw - 28);

        /* The cross. Two strokes, drawn rather than lettered for the same
         * reason as the arrows. */
        int cx = x + tw - 14, cy = XY_TABS_H / 2;
        int hot = inside(mx, my, cx - 6, cy - 6, 12, 12);
        if (hot) gui_fill(&xy_gui, cx - 6, cy - 6, 12, 12, C_HOVER);
        for (int k = -3; k <= 3; k++) {
            gui_fill(&xy_gui, cx + k, cy + k, 1, 1, hot ? C_TEXT : C_DIM);
            gui_fill(&xy_gui, cx + k, cy - k, 1, 1, hot ? C_TEXT : C_DIM);
        }
        hit(HIT_TAB_CLOSE, i, cx - 6, cy - 6, 12, 12);
        hit(HIT_TAB, i, x, 0, tw - 14, XY_TABS_H);
    }

    /* The plus. */
    int px = room + 4, py = (XY_TABS_H - 14) / 2;
    int hot = inside(mx, my, px, py, 20, 14);
    if (hot) gui_fill(&xy_gui, px, py, 20, 14, C_HOVER);
    gui_fill(&xy_gui, px + 9, py + 2, 2, 10, hot ? C_TEXT : C_DIM);
    gui_fill(&xy_gui, px + 5, py + 6, 10, 2, hot ? C_TEXT : C_DIM);
    hit(HIT_TAB_NEW, 0, px, py, 20, 14);
}

/* --- the toolbar --------------------------------------------------------- */

void xy_chrome_draw(gui_edit_t *address, int address_focused) {
    int mx = xy_gui.mx, my = xy_gui.my;
    xy_nhits = 0;

    draw_tabs(mx, my);

    int top = XY_TABS_H;
    gui_fill(&xy_gui, 0, top, xy_gui.w, XY_BAR_H, C_BAR);
    gui_fill(&xy_gui, 0, top + XY_BAR_H - 1, xy_gui.w, 1, C_EDGE);

    struct browser_window *bw = xy_tab_bw();
    int cy = top + XY_BAR_H / 2;

    int back_ok = bw && browser_window_history_back_available(bw);
    int fwd_ok  = bw && browser_window_history_forward_available(bw);
    int busy    = xy_ntabs > 0 && xy_win && xy_win->throbbing;

    struct { int kind, ok; } b[4] = {
        { HIT_BACK,    back_ok },
        { HIT_FORWARD, fwd_ok },
        { HIT_RELOAD,  bw != NULL },
        { HIT_HOME,    1 },
    };

    int x = 4;
    for (int i = 0; i < 4; i++) {
        int w = 28, h = XY_BAR_H - 6, y = top + 3;
        int hot = inside(mx, my, x, y, w, h);
        button(x, y, w, h, hot, b[i].ok);
        unsigned col = b[i].ok ? (hot ? C_TEXT : C_DIM) : C_EDGE;

        switch (b[i].kind) {
        case HIT_BACK:    arrow(x + w / 2, cy, 8, -1, col); break;
        case HIT_FORWARD: arrow(x + w / 2, cy, 8, +1, col); break;
        case HIT_RELOAD:
            if (busy) stop_glyph(x + w / 2, cy, 6, col);
            else      reload_glyph(x + w / 2, cy, 7, col);
            break;
        case HIT_HOME:    home_glyph(x + w / 2, cy, 7, col); break;
        }
        if (b[i].ok)
            hit(b[i].kind == HIT_RELOAD && busy ? HIT_STOP : b[i].kind,
                0, x, y, w, h);
        x += w + 2;
    }

    /* The address field takes the rest of the bar.
     *
     * While it does not have the focus it shows where you are, not what you
     * last typed -- an address bar that keeps a half-finished address after
     * you have gone somewhere else is telling you the wrong thing about the
     * page in front of you. Typing takes it over; leaving it gives it back. */
    if (!address_focused && xy_ntabs > 0 && xy_win)
        gui_edit_set(address, xy_win->url);

    int ax = x + 4, aw = xy_gui.w - ax - 6;
    gui_edit_draw(&xy_gui, address, ax, top + 4, aw, XY_BAR_H - 9,
                  address_focused, (int)((uptime_ms() / 500) & 1),
                  "type an address");
    hit(HIT_ADDRESS, 0, ax, top + 4, aw, XY_BAR_H - 9);
}

/* --- the message line ---------------------------------------------------- */

void xy_status_draw(void) {
    int y = xy_gui.h - XY_STATUS_H;
    gui_fill(&xy_gui, 0, y, xy_gui.w, XY_STATUS_H, C_BAR);
    gui_fill(&xy_gui, 0, y, xy_gui.w, 1, C_EDGE);

    const char *msg = "";
    if (xy_ntabs > 0 && xy_win) {
        msg = xy_win->status[0] ? xy_win->status : xy_win->url;
        if (xy_win->throbbing && xy_win->status[0] == 0) msg = "loading ...";
    }
    chrome_text(8, y, XY_STATUS_H, msg, C_DIM, xy_gui.w - 90);

    /* How far down the page, on the right, where a person looks for it. */
    if (xy_ntabs > 0 && xy_win) {
        int view = xy_gui.h - XY_CHROME_H - XY_STATUS_H;
        if (xy_win->ch > view) {
            int pct = (int)((long)xy_win->sy * 100 /
                            (xy_win->ch - view > 0 ? xy_win->ch - view : 1));
            if (pct > 100) pct = 100;
            char pc[16];
            snprintf(pc, sizeof pc, "%d%%", pct);
            chrome_text(xy_gui.w - 46, y, XY_STATUS_H, pc, C_DIM, 40);
        }
    }
}

int xy_chrome_hit(int x, int y, int *arg) {
    for (int i = 0; i < xy_nhits; i++) {
        struct hit *h = &xy_hits[i];
        if (inside(x, y, h->x, h->y, h->w, h->h)) {
            if (arg) *arg = h->arg;
            return h->kind;
        }
    }
    return HIT_NONE;
}
