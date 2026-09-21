/* The window table, and the tabs behind it.
 *
 * NetSurf keeps no scroll position of its own. It asks the frontend where the
 * viewport is and draws the page at that offset, so these few numbers are the
 * whole of the browser's idea of "where you are looking".
 *
 * A tab is one of these. NetSurf makes a browser_window and asks for a
 * gui_window to go with it; what the frontend does with that request is what
 * decides whether the browser has tabs at all. Here each request that carries
 * BW_CREATE_TAB, and each request made while there is room, becomes another
 * entry in the list -- so a link with target="_blank" opens a tab instead of
 * being lost, which is what it does in every other browser.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"

#include "xy_front.h"

struct gui_window *xy_tabs[XY_TABS_MAX];
int xy_ntabs;
int xy_tab;

struct browser_window *xy_tab_bw(void) {
    return (xy_ntabs > 0 && xy_tabs[xy_tab]) ? xy_tabs[xy_tab]->bw : NULL;
}

static struct gui_window *xyw_create(struct browser_window *bw,
                                     struct gui_window *existing,
                                     gui_window_create_flags flags) {
    (void)existing; (void)flags;

    if (xy_ntabs >= XY_TABS_MAX) {
        /* Full. The link still opens, in the tab it was clicked from --
         * better than refusing and losing it. */
        NSLOG(netsurf, INFO, "no room for another tab");
        if (xy_ntabs > 0) {
            xy_tabs[xy_tab]->bw = bw;
            return xy_tabs[xy_tab];
        }
        return NULL;
    }

    struct gui_window *g = calloc(1, sizeof *g);
    if (g == NULL) return NULL;
    g->bw = bw;
    snprintf(g->status, sizeof g->status, "ready");
    snprintf(g->title, sizeof g->title, "new tab");

    xy_tabs[xy_ntabs] = g;
    xy_tab = xy_ntabs;             /* a new tab comes to the front */
    xy_ntabs++;
    xy_damage();
    return g;
}

static void xyw_destroy(struct gui_window *g) {
    if (g == NULL) return;

    for (int i = 0; i < xy_ntabs; i++) {
        if (xy_tabs[i] != g) continue;
        for (int j = i; j + 1 < xy_ntabs; j++) xy_tabs[j] = xy_tabs[j + 1];
        xy_ntabs--;
        xy_tabs[xy_ntabs] = NULL;
        if (xy_tab >= xy_ntabs) xy_tab = xy_ntabs > 0 ? xy_ntabs - 1 : 0;
        break;
    }
    free(g);
    xy_damage();
}

/* Asks NetSurf to close one, which comes back round to destroy above. */
void xy_tab_close(int which) {
    if (which < 0 || which >= xy_ntabs) return;
    struct browser_window *bw = xy_tabs[which]->bw;
    if (bw != NULL) browser_window_destroy(bw);
}

static nserror xyw_invalidate(struct gui_window *g, const struct rect *rect) {
    (void)rect;   /* the whole viewport is redrawn; which part changed is not
                     worth tracking when the redraw is one pass over a buffer */
    /* Only the tab on screen can make the picture wrong. One loading in the
     * background changes nothing anybody can see. */
    if (xy_ntabs > 0 && g == xy_tabs[xy_tab]) xy_damage();
    return NSERROR_OK;
}

static bool xyw_get_scroll(struct gui_window *g, int *sx, int *sy) {
    if (g == NULL) return false;
    *sx = g->sx;
    *sy = g->sy;
    return true;
}

static nserror xyw_set_scroll(struct gui_window *g, const struct rect *r) {
    if (g == NULL) return NSERROR_BAD_PARAMETER;
    g->sx = r->x0;
    g->sy = r->y0;
    if (g->sx < 0) g->sx = 0;
    if (g->sy < 0) g->sy = 0;
    xy_damage();
    return NSERROR_OK;
}

static nserror xyw_get_dimensions(struct gui_window *g, int *width,
                                  int *height, bool scaled) {
    (void)g; (void)scaled;
    *width  = xy_gui.w;
    *height = xy_gui.h - XY_CHROME_H - XY_STATUS_H;
    if (*height < 1) *height = 1;
    return NSERROR_OK;
}

static void xyw_update_extent(struct gui_window *g) {
    if (g == NULL || g->bw == NULL) return;
    int w = 0, h = 0;
    if (browser_window_get_extents(g->bw, true, &w, &h) == NSERROR_OK) {
        g->cw = w;
        g->ch = h;
        xy_damage();          /* the scrollbar is drawn from this */
    }
}

static void xyw_set_title(struct gui_window *g, const char *title) {
    if (g == NULL) return;
    snprintf(g->title, sizeof g->title, "%s", title ? title : "");
    xy_damage();              /* the tab wears it */
}

static nserror xyw_set_url(struct gui_window *g, struct nsurl *url) {
    if (g == NULL) return NSERROR_BAD_PARAMETER;
    snprintf(g->url, sizeof g->url, "%s", url ? nsurl_access(url) : "");
    xy_damage();
    return NSERROR_OK;
}

static void xyw_set_status(struct gui_window *g, const char *text) {
    if (g == NULL) return;
    snprintf(g->status, sizeof g->status, "%s", text ? text : "");
    if (xy_ntabs > 0 && g == xy_tabs[xy_tab]) xy_damage();
}

static void xyw_set_pointer(struct gui_window *g, enum gui_pointer_shape s) {
    (void)g; (void)s;
    /* The window manager draws the pointer and this program does not get to
     * choose its shape. */
}

static void xyw_start_throbber(struct gui_window *g) {
    if (g) { g->throbbing = true; xy_damage(); }
}

static void xyw_stop_throbber(struct gui_window *g) {
    if (g) { g->throbbing = false; xy_damage(); }
}

static void xyw_place_caret(struct gui_window *g, int x, int y, int height,
                            const struct rect *clip) {
    (void)g; (void)x; (void)y; (void)height; (void)clip;
}

static void xyw_remove_caret(struct gui_window *g) {
    (void)g;
}

static struct gui_window_table window_table = {
    .create          = xyw_create,
    .destroy         = xyw_destroy,
    .invalidate      = xyw_invalidate,
    .get_scroll      = xyw_get_scroll,
    .set_scroll      = xyw_set_scroll,
    .get_dimensions  = xyw_get_dimensions,
    .update_extent   = xyw_update_extent,

    .set_title       = xyw_set_title,
    .set_url         = xyw_set_url,
    .set_status      = xyw_set_status,
    .set_pointer     = xyw_set_pointer,
    .start_throbber  = xyw_start_throbber,
    .stop_throbber   = xyw_stop_throbber,
    .place_caret     = xyw_place_caret,
    .remove_caret    = xyw_remove_caret,
};

struct gui_window_table *xy_window_table = &window_table;
