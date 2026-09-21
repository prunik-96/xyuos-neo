#!/usr/bin/env python3
"""Make the window table report when something has actually changed.

Everything here used to be a note taken and nothing else, because the loop
redrew the world every time round regardless. Now the loop asks first, so
each of these has to say when it has made the picture wrong.
"""
R = "/home/roman/xyuos-neo/"


def edit(path, tag, a, b, marker=None):
    s = open(R + path).read()
    if marker and marker in s:
        print("already: %s (%s)" % (path, tag))
        return
    if a not in s:
        raise SystemExit("MISSING in %s [%s]:\n%r" % (path, tag, a[:300]))
    open(R + path, "w").write(s.replace(a, b, 1))
    print("ok: %s (%s)" % (path, tag))


P = "third_party/nsxyuos/xy_window.c"

edit(P, "invalidate really invalidates",
     """static nserror xyw_invalidate(struct gui_window *g, const struct rect *rect) {
    (void)g; (void)rect;
    /* The whole surface is redrawn and blitted each time round the loop, so
     * there is nothing to record: a partial invalidation would be bookkeeping
     * that changed nothing. */
    return NSERROR_OK;
}""",
     """static nserror xyw_invalidate(struct gui_window *g, const struct rect *rect) {
    (void)g;
    (void)rect;   /* the whole viewport is redrawn; which part changed is not
                     worth tracking when the redraw is one pass over a buffer */
    xy_damage();
    return NSERROR_OK;
}""",
     "xy_damage();\n    return NSERROR_OK;\n}")

edit(P, "scrolling marks the picture wrong",
     """    g->sx = r->x0;
    g->sy = r->y0;
    if (g->sx < 0) g->sx = 0;
    if (g->sy < 0) g->sy = 0;
    return NSERROR_OK;""",
     """    g->sx = r->x0;
    g->sy = r->y0;
    if (g->sx < 0) g->sx = 0;
    if (g->sy < 0) g->sy = 0;
    xy_damage();
    return NSERROR_OK;""")

edit(P, "the extent",
     """    if (browser_window_get_extents(g->bw, true, &w, &h) == NSERROR_OK) {
        g->cw = w;
        g->ch = h;
    }""",
     """    if (browser_window_get_extents(g->bw, true, &w, &h) == NSERROR_OK) {
        g->cw = w;
        g->ch = h;
        xy_damage();          /* the scrollbar is drawn from this */
    }""")

for tag, a in (
    ("the title", """    snprintf(g->title, sizeof g->title, "%s", title ? title : "");"""),
    ("the address", """    snprintf(g->url, sizeof g->url, "%s", url ? nsurl_access(url) : "");"""),
    ("the message line", """    snprintf(g->status, sizeof g->status, "%s", text ? text : "");"""),
):
    edit(P, tag, a, a + "\n    xy_damage();")

edit(P, "the throbber",
     """static void xyw_start_throbber(struct gui_window *g) {
    if (g) g->throbbing = true;
}

static void xyw_stop_throbber(struct gui_window *g) {
    if (g) g->throbbing = false;
}""",
     """static void xyw_start_throbber(struct gui_window *g) {
    if (g) { g->throbbing = true; xy_damage(); }
}

static void xyw_stop_throbber(struct gui_window *g) {
    if (g) { g->throbbing = false; xy_damage(); }
}""")

print("--- the window table reports damage")
