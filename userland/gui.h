#ifndef GUI_H
#define GUI_H

/* A small GUI layer for xyuOS Neo programs.
 *
 * A program gets a window from the compositor for free -- title bar, borders,
 * minimise/maximise/close, dragging, snapping are all the WM's job. What it
 * gets here is the inside of that window: a pixel surface it owns, the
 * pointer and key events that landed in it, and enough drawing to build a
 * real interface out of.
 *
 * Deliberately immediate-mode. There is no widget tree, no retained state, no
 * callbacks: every frame the program paints what it wants and asks "was this
 * rectangle clicked?". For programs of this size that is far less machinery
 * than a retained toolkit, and it makes redraw-after-resize trivial -- there
 * is nothing to re-lay-out because nothing was laid out in the first place.
 *
 * Text is drawn from the window manager's own anti-aliased font, fetched once
 * through SYS_FONT as a coverage atlas. That is why the programs look like
 * they belong to the same system instead of each carrying a font of its own:
 * a 2 MB TrueType blob per binary was never going to fit on a RAM disk. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"
#include "../kernel/drivers/font8x8_basic.h"   /* fallback face only */

/* --- palette -------------------------------------------------------------
 * One place, so the file manager, the viewer and the task manager cannot
 * drift into looking like three different operating systems.
 *
 * These used to be compile-time constants, which is why a black desktop came
 * with a white task manager inside it: the theme reached the window frame,
 * drawn by the compositor, and stopped at the frame's inside edge. The colours
 * now come from the theme at run time -- fetched when a window opens and again
 * whenever the compositor says the pane changed, which is exactly what
 * switching theme makes it say.
 *
 * The names and their meanings are unchanged, so no drawing code had to move.
 * The values below are the light set, and they are what a program shows if the
 * fetch fails: a toolkit that cannot reach the compositor should still draw
 * something, not nothing. */
typedef struct {
    unsigned int win, panel, alt, text, dim, line, edge;
    unsigned int accent, accent2, sel, hot, btn, btndn;
    unsigned int bar, bar2, warn, good;
} gui_palette_t;

static gui_palette_t gui_pal = {
    0xF2F3F5u, 0xFFFFFFu, 0xF7F9FBu, 0x1B1B1Fu, 0x6B7280u, 0xD5D9DEu, 0xB9C0C8u,
    0x2A6FD6u, 0x1B4F9Cu, 0xCFE3FBu, 0xE6F0FBu, 0xECEEF1u, 0xD3DAE2u,
    0xE9ECEFu, 0xDDE2E7u, 0xC24A2Fu, 0x2E7D46u,
};

static inline void gui_palette_refresh(void) {
    xyuos_syscall3(SYS_SETTING, SETOP_PALETTE, (long)(void *)&gui_pal,
                   (long)sizeof gui_pal);
}

#define GC_WIN     (gui_pal.win)      /* window background          */
#define GC_PANEL   (gui_pal.panel)    /* content / list background  */
#define GC_ALT     (gui_pal.alt)      /* alternating row            */
#define GC_TEXT    (gui_pal.text)
#define GC_DIM     (gui_pal.dim)      /* secondary text             */
#define GC_LINE    (gui_pal.line)     /* hairline separators        */
#define GC_EDGE    (gui_pal.edge)     /* control borders            */
#define GC_ACCENT  (gui_pal.accent)   /* selection / focus          */
#define GC_ACCENT2 (gui_pal.accent2)
#define GC_SEL     (gui_pal.sel)      /* selected row background    */
#define GC_HOT     (gui_pal.hot)      /* hover                      */
#define GC_BTN     (gui_pal.btn)
#define GC_BTNDN   (gui_pal.btndn)
#define GC_BAR     (gui_pal.bar)      /* toolbars / status bars     */
#define GC_BAR2    (gui_pal.bar2)     /* the far stop of that gradient */
#define GC_WARN    (gui_pal.warn)
#define GC_GOOD    (gui_pal.good)

/* Glyph cells the window manager hands over; see kernel/gfx/font.h. */
#define GUI_FONT_SLOTS 448

typedef struct {
    unsigned int  *px;          /* ARGB surface, w*h                       */
    int            w, h;
    unsigned char *atlas;       /* GUI_FONT_SLOTS cells, fw*fh coverage    */
    int            fw, fh;      /* monospace cell size                     */
    int            mx, my;      /* last known pointer position             */
    unsigned char  mb;          /* buttons held right now                  */
    int            lost;        /* consecutive frames with no window       */
} gui_t;

/* One input event, whichever kind arrived. */
#define GE_NONE  0
#define GE_KEY   1
#define GE_MOUSE 2

typedef struct {
    int             type;
    key_event_t     k;
    mouse_event_t   m;
} gui_event_t;

/* --- colour helpers ------------------------------------------------------ */

static inline unsigned gui_mix(unsigned a, unsigned b, int t) {  /* t: 0..255 */
    unsigned ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    unsigned it = 255u - (unsigned)t;
    return (((ar * it + br * (unsigned)t) / 255) << 16) |
           (((ag * it + bg * (unsigned)t) / 255) << 8) |
           ((ab * it + bb * (unsigned)t) / 255);
}

/* --- surface lifecycle --------------------------------------------------- */

/* Re-query the window interior and resize the surface to match. Returns 0 if
 * there is nothing to draw into (window collapsed, or out of memory). */
static inline int gui_sync(gui_t *g) {
    int w = 0, h = 0;
    gfx_info(&w, &h);
    if (w <= 0 || h <= 0) { g->lost++; return 0; }
    if (!g->px || w != g->w || h != g->h) {
        unsigned int *n = (unsigned int *)realloc(g->px, (size_t)w * h * 4);
        if (!n) { g->lost++; return 0; }
        g->px = n;
        g->w = w;
        g->h = h;
    }
    g->lost = 0;
    return 1;
}

/* Has the window gone for good?
 *
 * A single failed sync means almost nothing: the window may be mid-resize, or
 * memory may be tight for a moment. Treating that as "quit" is how a program
 * ends up vanishing for no reason the user can see. Only a long run of
 * failures means the window is really gone and the program should stop. */
static inline int gui_lost(gui_t *g) { return g->lost > 40; }

static inline int gui_open(gui_t *g) {
    gui_palette_refresh();
    memset(g, 0, sizeof *g);
    if (font_cell(&g->fw, &g->fh) && g->fw > 0 && g->fh > 0) {
        unsigned long need = (unsigned long)g->fw * g->fh * GUI_FONT_SLOTS;
        g->atlas = (unsigned char *)malloc(need);
        if (g->atlas && font_atlas(g->atlas, need) < 0) {
            free(g->atlas);
            g->atlas = 0;
        }
    }
    if (!g->atlas) { g->fw = 8; g->fh = 8; }   /* fall back to the bitmap face */
    return gui_sync(g);
}

static inline void gui_close(gui_t *g) {
    gfx_end();
    free(g->px);
    free(g->atlas);
    g->px = 0;
    g->atlas = 0;
}

static inline void gui_present(gui_t *g) {
    if (g->px) gfx_blit(g->px, g->w, g->h);
}

/* --- input --------------------------------------------------------------- */

/* Drain one event. Pointer events are taken first: when the user is dragging
 * a scrollbar, motion must not queue up behind whatever they typed earlier. */
static inline int gui_poll(gui_t *g, gui_event_t *e) {
    if (poll_mouse(&e->m)) {
        e->type = GE_MOUSE;
        g->mx = e->m.x;
        g->my = e->m.y;
        g->mb = e->m.buttons;
        return 1;
    }
    if (poll_event(&e->k)) {
        e->type = GE_KEY;
        /* A resize is also how a theme change reaches a program: the
         * compositor sends one to every pane when the layout or the look
         * changes, so this is the moment to pick the colours up again. */
        if (e->k.code == XKEY_RESIZE) gui_palette_refresh();
        return 1;
    }
    e->type = GE_NONE;
    return 0;
}

static inline int gui_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* Does this pointer event change anything on screen?
 *
 * Motion on its own usually does not: the pointer itself is drawn by the
 * compositor, not by the program. Repainting a whole window because the mouse
 * crossed it is how a viewer ends up redrawing a scaled photograph sixty
 * times a second while the user is doing nothing -- which reads as lag and,
 * with the compositor repainting behind it, as flicker. So a program marks
 * itself dirty for buttons, the wheel, and for motion ONLY when the thing
 * under the pointer changed. */
static inline int gui_acts(const gui_event_t *e) {
    return e->m.pressed || e->m.released || e->m.wheel;
}

/* Did this event click inside the rectangle? */
static inline int gui_clicked(const gui_event_t *e, int x, int y, int w, int h) {
    return e->type == GE_MOUSE && (e->m.pressed & MB_LEFT) &&
           gui_in(e->m.x, e->m.y, x, y, w, h);
}

/* --- primitives ----------------------------------------------------------
 * Everything clips to the surface, so a program may draw wherever it likes
 * and simply let the edges fall off. */

static inline void gui_fill(gui_t *g, int x, int y, int w, int h, unsigned c) {
    if (!g->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g->w) w = g->w - x;
    if (y + h > g->h) h = g->h - y;
    for (int j = 0; j < h; j++) {
        unsigned int *row = g->px + (size_t)(y + j) * g->w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static inline void gui_clear(gui_t *g, unsigned c) {
    gui_fill(g, 0, 0, g->w, g->h, c);
}

/* Blend `c` over the surface at `t`/255 opacity. */
static inline void gui_tint(gui_t *g, int x, int y, int w, int h,
                            unsigned c, int t) {
    if (!g->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g->w) w = g->w - x;
    if (y + h > g->h) h = g->h - y;
    for (int j = 0; j < h; j++) {
        unsigned int *row = g->px + (size_t)(y + j) * g->w + x;
        for (int i = 0; i < w; i++) row[i] = gui_mix(row[i], c, t);
    }
}

static inline void gui_rect(gui_t *g, int x, int y, int w, int h, unsigned c) {
    if (w <= 0 || h <= 0) return;
    gui_fill(g, x, y, w, 1, c);
    gui_fill(g, x, y + h - 1, w, 1, c);
    gui_fill(g, x, y, 1, h, c);
    gui_fill(g, x + w - 1, y, 1, h, c);
}

/* Vertical gradient -- the one flourish that makes a flat fill read as a
 * surface rather than a hole. */
static inline void gui_vgrad(gui_t *g, int x, int y, int w, int h,
                             unsigned top, unsigned bot) {
    if (h <= 0) return;
    for (int j = 0; j < h; j++)
        gui_fill(g, x, y + j, w, 1, gui_mix(top, bot, h > 1 ? j * 255 / (h - 1) : 0));
}

/* A rectangle with the corner pixels knocked out: enough to read as rounded
 * at this scale, and far cheaper than real anti-aliased arcs. */
static inline void gui_panel(gui_t *g, int x, int y, int w, int h,
                             unsigned fill, unsigned edge) {
    if (w < 4 || h < 4) { gui_fill(g, x, y, w, h, fill); return; }
    gui_fill(g, x + 1, y, w - 2, h, fill);
    gui_fill(g, x, y + 1, 1, h - 2, fill);
    gui_fill(g, x + w - 1, y + 1, 1, h - 2, fill);
    if (edge != fill) {
        gui_fill(g, x + 1, y, w - 2, 1, edge);
        gui_fill(g, x + 1, y + h - 1, w - 2, 1, edge);
        gui_fill(g, x, y + 1, 1, h - 2, edge);
        gui_fill(g, x + w - 1, y + 1, 1, h - 2, edge);
    }
}

/* --- text ----------------------------------------------------------------
 * Glyphs come from the WM's coverage atlas when there is one, and from the
 * built-in 8x8 face when there is not. `scale` replicates pixels, which is
 * ugly enough at 3x that nothing here asks for more than 2. */

/* The same three ranges the window manager rasterises, at the same slots.
 * These two tables have to agree; a mismatch does not fail, it just draws the
 * wrong letters. */

static inline int gui_font_slot(int cp) {
    if (cp >= 0x20  && cp <= 0x7E)  return cp;
    if (cp >= 0xA0  && cp <= 0xFF)  return 128 + (cp - 0xA0);
    if (cp >= 0x100 && cp <= 0x17F) return 224 + (cp - 0x100);
    if (cp >= 0x400 && cp <= 0x45F) return 352 + (cp - 0x400);
    return -1;
}

/* One UTF-8 sequence; returns the bytes consumed, never zero. */
static inline int gui_utf8(const char *s, int *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int need, v;
    if      ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else { *cp = '?'; return 1; }
    for (int i = 1; i <= need; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xC0) != 0x80) { *cp = '?'; return 1; }
        v = (v << 6) | (k & 0x3F);
    }
    *cp = v;
    return need + 1;
}

/* Characters in a UTF-8 string -- what a width in cells is measured in. */
static inline int gui_len(const char *s) {
    int n = 0;
    for (int i = 0; s[i]; n++) { int cp; i += gui_utf8(s + i, &cp); }
    return n;
}

static inline void gui_glyph(gui_t *g, int x, int y, int cp,
                             unsigned fg, int scale) {
    if (!g->px || scale < 1) return;
    int slot = gui_font_slot(cp);
    if (slot < 0) slot = '?';
    unsigned c = (unsigned)(g->atlas ? slot : (slot & 0x7F));

    for (int cy = 0; cy < g->fh; cy++) {
        for (int cx = 0; cx < g->fw; cx++) {
            unsigned cov;
            if (g->atlas) {
                cov = g->atlas[(size_t)c * g->fw * g->fh + (size_t)cy * g->fw + cx];
            } else {
                /* font8x8_basic is MSB-leftmost despite what its header says. */
                cov = (font8x8_basic[c][cy] & (0x80u >> cx)) ? 255u : 0u;
            }
            if (!cov) continue;
            int px0 = x + cx * scale, py0 = y + cy * scale;
            for (int sy = 0; sy < scale; sy++) {
                int py = py0 + sy;
                if (py < 0 || py >= g->h) continue;
                unsigned int *row = g->px + (size_t)py * g->w;
                for (int sx = 0; sx < scale; sx++) {
                    int px = px0 + sx;
                    if (px < 0 || px >= g->w) continue;
                    row[px] = cov == 255 ? fg : gui_mix(row[px], fg, (int)cov);
                }
            }
        }
    }
}

static inline int gui_tw(gui_t *g, const char *s, int scale) {
    return gui_len(s) * g->fw * scale;
}

static inline void gui_text_s(gui_t *g, int x, int y, const char *s,
                              unsigned fg, int scale) {
    for (int i = 0; s[i]; ) {
        int cp;
        i += gui_utf8(s + i, &cp);
        if (x >= g->w) break;
        if (cp != ' ') gui_glyph(g, x, y, cp, fg, scale);
        x += g->fw * scale;
    }
}

static inline void gui_text(gui_t *g, int x, int y, const char *s, unsigned fg) {
    gui_text_s(g, x, y, s, fg, 1);
}

/* Draw `s` truncated with an ellipsis to fit `maxw` pixels -- what a file
 * list needs for every name that is longer than its column. */
static inline void gui_text_clip(gui_t *g, int x, int y, const char *s,
                                 unsigned fg, int maxw) {
    int cells = maxw / g->fw;
    if (cells <= 0) return;
    if (gui_len(s) <= cells) { gui_text(g, x, y, s, fg); return; }
    if (cells <= 3) return;
    int col = 0;
    for (int i = 0; s[i] && col < cells - 3; col++) {
        int cp;
        i += gui_utf8(s + i, &cp);
        gui_glyph(g, x + col * g->fw, y, cp, fg, 1);
    }
    for (int i = 0; i < 3; i++) gui_glyph(g, x + (cells - 3 + i) * g->fw, y, '.', fg, 1);
}

static inline void gui_text_center(gui_t *g, int x, int y, int w,
                                   const char *s, unsigned fg) {
    int tw = gui_tw(g, s, 1);
    gui_text(g, x + (w - tw) / 2, y, s, fg);
}

/* --- controls ------------------------------------------------------------ */

#define GB_NORMAL 0
#define GB_HOVER  1
#define GB_DOWN   2
#define GB_OFF    3

static inline void gui_button(gui_t *g, int x, int y, int w, int h,
                              const char *label, int state) {
    unsigned top = GC_PANEL, bot = GC_BTN, txt = GC_TEXT;
    if (state == GB_HOVER) { top = 0xFFFFFF; bot = GC_HOT; }
    if (state == GB_DOWN)  { top = GC_BTNDN; bot = GC_BTNDN; }
    if (state == GB_OFF)   { top = GC_BTN; bot = GC_BTN; txt = GC_DIM; }
    gui_vgrad(g, x + 1, y + 1, w - 2, h - 2, top, bot);
    gui_rect(g, x, y, w, h, state == GB_HOVER ? GC_ACCENT : GC_EDGE);
    gui_text_center(g, x, y + (h - g->fh) / 2, w, label, txt);
}

/* A vertical scrollbar for a list of `total` items showing `vis` of them from
 * `first`. Draws nothing when everything already fits. */
static inline void gui_scrollbar(gui_t *g, int x, int y, int w, int h,
                                 int first, int vis, int total) {
    if (total <= vis || h <= 0) return;
    gui_fill(g, x, y, w, h, GC_BAR);
    int th = h * vis / total;
    if (th < 18) th = 18;
    if (th > h) th = h;
    int span = total - vis;
    int ty = y + (span > 0 ? (h - th) * first / span : 0);
    gui_panel(g, x + 2, ty, w - 4, th, GC_EDGE, GC_DIM);
}

/* Map a click at `py` on that scrollbar to a new `first` index. */
static inline int gui_scrollbar_pick(int y, int h, int py, int vis, int total) {
    if (total <= vis || h <= 0) return 0;
    int f = (py - y) * total / h - vis / 2;
    if (f > total - vis) f = total - vis;
    if (f < 0) f = 0;
    return f;
}

/* --- single-line text field ----------------------------------------------
 * Used by every rename / save-as / search box in the system, which is exactly
 * why it lives here and not in one of them. */

typedef struct {
    char *buf;
    int   max;      /* buffer size including the terminator */
    int   len;
    int   cur;      /* caret position, 0..len */
} gui_edit_t;

static inline void gui_edit_set(gui_edit_t *e, const char *s) {
    int n = 0;
    if (s) for (; s[n] && n < e->max - 1; n++) e->buf[n] = s[n];
    e->buf[n] = 0;
    e->len = n;
    e->cur = n;
}

/* Feed one key. Returns 1 if the text changed. */
static inline int gui_edit_key(gui_edit_t *e, const key_event_t *k) {
    if (k->code == XKEY_LEFT)  { if (e->cur > 0) e->cur--; return 0; }
    if (k->code == XKEY_RIGHT) { if (e->cur < e->len) e->cur++; return 0; }
    if (k->code == XKEY_HOME)  { e->cur = 0; return 0; }
    if (k->code == XKEY_END)   { e->cur = e->len; return 0; }
    if (k->code == XKEY_BKSP) {
        if (e->cur == 0) return 0;
        memmove(e->buf + e->cur - 1, e->buf + e->cur, (size_t)(e->len - e->cur + 1));
        e->cur--; e->len--;
        return 1;
    }
    if (k->code == XKEY_DEL) {
        if (e->cur >= e->len) return 0;
        memmove(e->buf + e->cur, e->buf + e->cur + 1, (size_t)(e->len - e->cur));
        e->len--;
        return 1;
    }
    if (k->code != XKEY_CHAR) return 0;
    unsigned char c = (unsigned char)k->ascii;
    if (c < 32 || c > 126) return 0;
    if (e->len >= e->max - 1) return 0;
    memmove(e->buf + e->cur + 1, e->buf + e->cur, (size_t)(e->len - e->cur + 1));
    e->buf[e->cur++] = (char)c;
    e->len++;
    return 1;
}

/* `focus` draws the caret; `blink` is normally (uptime_ms() / 500) & 1. */
static inline void gui_edit_draw(gui_t *g, gui_edit_t *e, int x, int y,
                                 int w, int h, int focus, int blink,
                                 const char *placeholder) {
    gui_panel(g, x, y, w, h, GC_PANEL, focus ? GC_ACCENT : GC_EDGE);
    int ty = y + (h - g->fh) / 2;
    int cells = (w - 10) / g->fw;
    if (cells < 1) return;
    int from = 0;
    if (e->cur > cells - 1) from = e->cur - (cells - 1);
    if (e->len == 0 && placeholder && !focus) {
        gui_text(g, x + 6, ty, placeholder, GC_DIM);
        return;
    }
    for (int i = 0; i < cells && from + i < e->len; i++)
        gui_glyph(g, x + 6 + i * g->fw, ty, (unsigned char)e->buf[from + i], GC_TEXT, 1);
    if (focus && blink) {
        int cx = x + 6 + (e->cur - from) * g->fw;
        gui_fill(g, cx, ty, 2, g->fh, GC_ACCENT);
    }
}

#endif /* GUI_H */
