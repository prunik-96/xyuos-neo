/* The xyuOS frontend for NetSurf: what its parts share.
 *
 * xyuOS gives a program one thing to draw with -- a block of ARGB pixels and
 * a call that puts it on the screen. Everything below is software rendering
 * into that block. There is no accelerated path to miss, and no windowing
 * toolkit underneath, so the frontend is the toolkit: the plotters draw, the
 * window table keeps the scroll position, and one loop turns key and mouse
 * events into NetSurf calls.
 *
 * The frontend deliberately owns exactly one browser window. The window
 * manager already tiles, so a second one here would be a worse copy of what
 * the system does better.
 */
#ifndef XY_FRONT_H
#define XY_FRONT_H

#include <stdbool.h>
#include <stddef.h>

/* First: every table below is a struct of functions returning nserror, so
 * this has to be in scope before any of them are read. */
#include "utils/errors.h"

#include "netsurf/types.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "netsurf/bitmap.h"
#include "netsurf/misc.h"
#include "netsurf/layout.h"
#include "netsurf/fetch.h"

/* xyuOS's own window and drawing. gui.h is a header of static definitions,
 * so it is included once here and the parts that need it say so. */
#include "gui.h"

/* The chrome, in three pieces: a strip of tabs, a toolbar under it, and a
 * message line along the bottom. The page gets what is left.
 *
 * These are pixels rather than rows of text on purpose. The window manager's
 * font is anti-aliased and its cell size is whatever it is; laying the chrome
 * out in its cells would make the toolbar change height with the theme. */
#define XY_TABS_H   24
#define XY_BAR_H    28
#define XY_CHROME_H (XY_TABS_H + XY_BAR_H)
#define XY_STATUS_H 20

extern gui_t xy_gui;               /* the surface everything is drawn into */

struct browser_window;

/* NetSurf's opaque per-window handle, which is ours to define. */
struct gui_window {
    struct browser_window *bw;
    int  sx, sy;                   /* scroll position, in page pixels */
    int  cw, ch;                   /* the page's full extent */
    bool throbbing;
    char title[160];
    char status[256];
    char url[1024];
};

/* Tabs. NetSurf makes one browser_window per tab and asks the frontend for a
 * gui_window to go with it; the frontend decides what that means. Here it
 * means an entry in this list, of which exactly one is on screen. */
#define XY_TABS_MAX 8

extern struct gui_window *xy_tabs[XY_TABS_MAX];
extern int xy_ntabs;
extern int xy_tab;                 /* which one is showing */

/* The tab on screen, or NULL before the first one opens. Everything that
 * used to be written against a single window still reads this. */
#define xy_win (xy_tabs[xy_tab])

/* Made by the window table when NetSurf asks for a window; the frontend
 * reaches for this when the person asks for a tab. */
struct browser_window;
int  xy_tab_open(const char *url);     /* a new tab; 1 if it was made */
void xy_tab_close(int which);
struct browser_window *xy_tab_bw(void);

/* Set whenever anything that is drawn has changed. The loop redraws only
 * when it is set.
 *
 * This is not a refinement, it is the difference between a browser and a
 * slideshow. Redrawing unconditionally means running NetSurf's whole layout
 * traversal and software-plotting every pixel of the page fifty times a
 * second, on an emulated processor, WHILE trying to download -- and the
 * download only advances when this same loop gets round to polling it. The
 * page then takes twenty seconds to arrive, and almost all of that time went
 * into drawing the half-finished page over and over. */
extern bool xy_dirty;

static inline void xy_damage(void) { xy_dirty = true; }

/* --- what the plotters need --------------------------------------------- */

extern const struct plotter_table xy_plotters;

/* The clip rectangle in force, in surface coordinates. Plotting outside it
 * does nothing. */
extern int xy_cx0, xy_cy0, xy_cx1, xy_cy1;

void xy_clip_reset(void);

/* NetSurf stores a colour with red in the low byte; this surface wants it in
 * the high one. */
static inline unsigned xy_colour(colour c) {
    return ((c & 0x0000FFu) << 16) | (c & 0x00FF00u) | ((c >> 16) & 0xFFu);
}

/* How many times the bitmap font is magnified for a given style. Layout and
 * drawing both go through this, so what is measured is what is drawn --
 * which is the whole reason a font this simple is workable. */
int xy_font_scale(const plot_font_style_t *fstyle);

/* Where the baseline sits inside a cell of that size. */
int xy_font_ascent(int scale);

/* --- the tables ---------------------------------------------------------- */

extern struct gui_window_table   *xy_window_table;
extern struct gui_bitmap_table   *xy_bitmap_table;
extern struct gui_layout_table   *xy_layout_table;
extern struct gui_fetch_table    *xy_fetch_table;
extern struct gui_misc_table      xy_misc_table;

/* The scheduler behind guit->misc->schedule. Called from the event loop. */
void  xy_schedule_run(void);
int   xy_schedule_next_ms(void);

#endif
