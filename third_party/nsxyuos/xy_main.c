/* NetSurf on xyuOS: starting it, drawing it, and one loop in the middle.
 *
 * The loop is the whole program. NetSurf never blocks and never waits: it
 * asks to be called back and returns. So each turn does four things in the
 * same order -- run whatever the schedule says is due, take any key or click
 * the window manager has for us, draw if anything changed, and then wait no
 * longer than the next scheduled callback allows.
 *
 * "if anything changed" is doing real work there. Drawing unconditionally
 * means running NetSurf's whole layout traversal and software-plotting every
 * pixel fifty times a second whether or not the picture is different.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"

#include "netsurf/netsurf.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "netsurf/misc.h"
#include "netsurf/plotters.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"

#include "xy_front.h"
#include "xy_chrome.h"
#include "fetch_xyuos.h"
#include "xy_image.h"

gui_t xy_gui;
bool  xy_dirty = true;             /* draw once at the start */

static bool running = true;

/* The address line, which is edited in place in the toolbar. */
static char       address_buf[1024];
static gui_edit_t address = { address_buf, sizeof address_buf, 0, 0 };
static bool address_focused = false;

#define HOME_PAGE "about:welcome"

/* --- options ------------------------------------------------------------- */

static nserror set_defaults(struct nsoption_s *defaults) {
    (void)defaults;

    /* Nothing is written to disc: this system's storage is an image of a
     * hundred and twenty-eight megabytes, and NetSurf's own default for the
     * disc cache is a gigabyte. The memory cache is what there is. */
    nsoption_set_int(memory_cache_size, 8 * 1024 * 1024);
    nsoption_set_uint(disc_cache_size, 0);

    /* One fetch runs at a time down in the kernel, so a page that gave up
     * after one try would abandon images that were only ever queued. */
    nsoption_set_uint(max_retried_fetches, 2);

    /* Run scripts. NetSurf defaults this off, because a build without an
     * engine would otherwise pretend to have one; this build has Duktape and
     * the generated DOM bindings linked in, so it can say yes. */
    nsoption_set_bool(enable_javascript, true);

    /* The text size a page gets when it does not choose one, in tenths of
     * a point: 12pt, which at the 96 DPI set in main is the 16 pixels every
     * other browser uses. NetSurf's 12.8 was chosen for its own 90. */
    nsoption_set_int(font_size, 120);

    return NSERROR_OK;
}

/* --- where the page lives ------------------------------------------------ */

#define PAGE_TOP    XY_CHROME_H
#define PAGE_BOTTOM (xy_gui.h - XY_STATUS_H)
#define PAGE_H      (PAGE_BOTTOM - PAGE_TOP)

static void draw_page(void) {
    struct redraw_context ctx = {
        .interactive = true,
        .background_images = true,
        .plot = &xy_plotters,
    };
    struct rect clip = { 0, PAGE_TOP, xy_gui.w, PAGE_BOTTOM };

    gui_fill(&xy_gui, 0, PAGE_TOP, xy_gui.w, PAGE_H, 0xFFFFFFu);

    struct browser_window *bw = xy_tab_bw();
    if (bw == NULL || xy_win == NULL) return;

    /* How tall the page is, asked for here rather than only when NetSurf
     * announces it: the scrollbar and the limit on scrolling are read every
     * frame, and a stale zero means the page cannot be scrolled at all. */
    int w = 0, h = 0;
    if (browser_window_get_extents(bw, true, &w, &h) == NSERROR_OK) {
        xy_win->cw = w;
        xy_win->ch = h;
    }

    xy_cx0 = clip.x0; xy_cy0 = clip.y0;
    xy_cx1 = clip.x1; xy_cy1 = clip.y1;

    browser_window_redraw(bw, -xy_win->sx, PAGE_TOP - xy_win->sy, &clip, &ctx);
    xy_clip_reset();

    if (xy_win->ch > PAGE_H)
        gui_scrollbar(&xy_gui, xy_gui.w - 8, PAGE_TOP, 8, PAGE_H,
                      xy_win->sy, PAGE_H, xy_win->ch);
}

/* --- navigation ---------------------------------------------------------- */

static void go_to(const char *text) {
    if (text == NULL || text[0] == 0) return;

    /* A bare host is what people type. Guessing http:// for it is the one
     * guess a browser is expected to make. */
    char buf[1200];
    if (strstr(text, "://") == NULL && strncmp(text, "about:", 6) != 0)
        snprintf(buf, sizeof buf, "http://%s", text);
    else
        snprintf(buf, sizeof buf, "%s", text);

    nsurl *url;
    if (nsurl_create(buf, &url) != NSERROR_OK) {
        if (xy_win) snprintf(xy_win->status, sizeof xy_win->status,
                             "that is not an address we can read");
        xy_damage();
        return;
    }

    struct browser_window *bw = xy_tab_bw();
    if (bw == NULL) {
        struct browser_window *made = NULL;
        browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &made);
    } else {
        browser_window_navigate(bw, url, NULL, BW_NAVIGATE_HISTORY,
                                NULL, NULL, NULL);
    }
    nsurl_unref(url);
    xy_damage();
}

int xy_tab_open(const char *where) {
    nsurl *url = NULL;
    if (nsurl_create(where ? where : HOME_PAGE, &url) != NSERROR_OK)
        return 0;

    struct browser_window *made = NULL;
    nserror e = browser_window_create(BW_CREATE_HISTORY | BW_CREATE_TAB,
                                      url, NULL, xy_tab_bw(), &made);
    nsurl_unref(url);
    xy_damage();
    return e == NSERROR_OK;
}

static void scroll_by(int dy) {
    if (xy_win == NULL) return;
    int max = xy_win->ch - PAGE_H;
    if (max < 0) max = 0;
    int was = xy_win->sy;
    xy_win->sy += dy;
    if (xy_win->sy < 0) xy_win->sy = 0;
    if (xy_win->sy > max) xy_win->sy = max;
    if (xy_win->sy != was) xy_damage();
}

/* --- what the buttons do ------------------------------------------------- */

static void act(int what, int arg) {
    struct browser_window *bw = xy_tab_bw();

    switch (what) {
    case HIT_BACK:
        if (bw && browser_window_history_back_available(bw))
            browser_window_history_back(bw, false);
        break;
    case HIT_FORWARD:
        if (bw && browser_window_history_forward_available(bw))
            browser_window_history_forward(bw, false);
        break;
    case HIT_RELOAD:
        if (bw) browser_window_reload(bw, false);
        break;
    case HIT_STOP:
        if (bw) browser_window_stop(bw);
        break;
    case HIT_HOME:
        go_to(HOME_PAGE);
        break;
    case HIT_ADDRESS:
        address_focused = true;
        if (xy_win) gui_edit_set(&address, xy_win->url);
        break;
    case HIT_TAB:
        if (arg >= 0 && arg < xy_ntabs) xy_tab = arg;
        break;
    case HIT_TAB_CLOSE:
        xy_tab_close(arg);
        break;
    case HIT_TAB_NEW:
        xy_tab_open(HOME_PAGE);
        break;
    default:
        return;
    }
    xy_damage();
}

/* --- input --------------------------------------------------------------- */

static void on_key(const key_event_t *k) {
    if (address_focused) {
        if (k->code == XKEY_ENTER) {
            address_focused = false;
            go_to(address.buf);
            return;
        }
        if (k->code == XKEY_ESC) { address_focused = false; return; }
        gui_edit_key(&address, k);
        return;
    }

    struct browser_window *bw = xy_tab_bw();

    if (k->code == XKEY_CHAR) {
        switch (k->ascii) {
        case 'l': case '/': act(HIT_ADDRESS, 0);  return;
        case 'r':           act(HIT_RELOAD, 0);   return;
        case 'b':           act(HIT_BACK, 0);     return;
        case 'f':           act(HIT_FORWARD, 0);  return;
        case 'h':           act(HIT_HOME, 0);     return;
        case 't':           act(HIT_TAB_NEW, 0);  return;
        case 'w':           xy_tab_close(xy_tab); return;
        case ']':
            if (xy_ntabs > 1) { xy_tab = (xy_tab + 1) % xy_ntabs; xy_damage(); }
            return;
        case '[':
            if (xy_ntabs > 1) {
                xy_tab = (xy_tab + xy_ntabs - 1) % xy_ntabs;
                xy_damage();
            }
            return;
        case ' ':           scroll_by(PAGE_H - 40); return;
        default:
            if (bw) browser_window_key_press(bw,
                        (uint32_t)(unsigned char)k->ascii);
            return;
        }
    }

    switch (k->code) {
    case XKEY_ESC:    running = false;        break;
    case XKEY_UP:     scroll_by(-40);         break;
    case XKEY_DOWN:   scroll_by(40);          break;
    case XKEY_PGUP:   scroll_by(-(PAGE_H - 40)); break;
    case XKEY_PGDN:   scroll_by(PAGE_H - 40);    break;
    case XKEY_HOME:   if (xy_win) { xy_win->sy = 0; xy_damage(); } break;
    case XKEY_END:    scroll_by(xy_win ? xy_win->ch : 0); break;
    case XKEY_RESIZE: xy_clip_reset(); xy_damage(); break;
    default: break;
    }
}

static void on_mouse(const mouse_event_t *m) {
    /* The wheel scrolls wherever the pointer is. Three lines a notch. */
    if (m->wheel != 0) {
        scroll_by(-m->wheel * 3 * xy_gui.fh);
        return;
    }

    /* Moving over the chrome changes what is highlighted, so it has to be
     * redrawn -- but only while the pointer is actually up there. */
    if (m->y < XY_CHROME_H || m->y >= PAGE_BOTTOM) {
        static int was_over;
        if (!was_over) { was_over = 1; xy_damage(); }
        else if (m->x != xy_gui.mx || m->y != xy_gui.my) xy_damage();

        if (m->pressed) {
            int arg = 0;
            int what = xy_chrome_hit(m->x, m->y, &arg);
            if (what != HIT_ADDRESS && address_focused) address_focused = false;
            act(what, arg);
        }
        return;
    }

    struct browser_window *bw = xy_tab_bw();
    if (bw == NULL || xy_win == NULL) return;

    if (address_focused) { address_focused = false; xy_damage(); }

    int px = m->x + xy_win->sx;
    int py = m->y - PAGE_TOP + xy_win->sy;

    /* NetSurf wants the press and the release separately, and it is the
     * release that follows a link -- a press alone starts a drag. */
    if (m->pressed & MB_LEFT)        browser_window_mouse_click(bw, BROWSER_MOUSE_PRESS_1, px, py);
    else if (m->pressed & MB_RIGHT)  browser_window_mouse_click(bw, BROWSER_MOUSE_PRESS_2, px, py);
    else if (m->released & MB_LEFT)  browser_window_mouse_click(bw, BROWSER_MOUSE_CLICK_1, px, py);
    else if (m->released & MB_RIGHT) browser_window_mouse_click(bw, BROWSER_MOUSE_CLICK_2, px, py);
    else if (m->buttons == 0)        browser_window_mouse_track(bw, BROWSER_MOUSE_HOVER, px, py);
    else                             browser_window_mouse_track(bw, BROWSER_MOUSE_DRAG_ON, px, py);
}

/* --- the loop ------------------------------------------------------------ */

static void run(void) {
    unsigned last_blink = 0;

    while (running) {
        xy_schedule_run();

        gui_event_t e;
        while (gui_poll(&xy_gui, &e)) {
            if (e.type == GE_KEY)   { on_key(&e.k); xy_damage(); }
            if (e.type == GE_MOUSE) { on_mouse(&e.m); }
            if (!running) return;
        }

        if (gui_lost(&xy_gui)) { running = false; break; }

        /* The caret blinks and nothing else changes on a timer, so the clock
         * only forces a redraw while the address field has the focus. */
        if (address_focused) {
            unsigned phase = uptime_ms() / 500;
            if (phase != last_blink) { last_blink = phase; xy_damage(); }
        }

        if (xy_dirty) {
            xy_dirty = false;
            draw_page();
            xy_chrome_draw(&address, address_focused);
            xy_status_draw();
            gui_present(&xy_gui);
        }

        int wait = xy_schedule_next_ms();
        if (wait > 20) wait = 20;      /* the window must stay answerable */
        if (wait > 0) sleep_ms((unsigned)wait);
    }
}

void fetch_xyuos_shutdown(void);

int main(int argc, char **argv) {
    struct netsurf_table xyuos_table = {
        .misc   = &xy_misc_table,
        .window = xy_window_table,
        .fetch  = xy_fetch_table,
        .bitmap = xy_bitmap_table,
        .layout = xy_layout_table,
    };

    if (netsurf_register(&xyuos_table) != NSERROR_OK) {
        printf("netsurf: the operation tables were refused\n");
        return 1;
    }

    if (nsoption_init(set_defaults, &nsoptions, &nsoptions_default)
            != NSERROR_OK) {
        printf("netsurf: options failed to start\n");
        return 1;
    }

    /* NetSurf's own text, if it is on the disc. Without it every message
     * comes out as its key, which is ugly and not fatal. */
    if (messages_add_from_file("/lib/netsurf/Messages") != NSERROR_OK)
        NSLOG(netsurf, INFO, "no Messages file; keys will be shown instead");

    if (netsurf_init(NULL) != NSERROR_OK) {
        printf("netsurf: failed to start\n");
        return 1;
    }

    /* A CSS pixel is a screen pixel, and a point is 1/72 of 96 of them, as
     * in every current browser. NetSurf's own default is 90, which makes
     * 12pt text 15 pixels instead of the 16 a page expects. */
    browser_set_dpi(96);
    xy_text_init();

    /* The http and https fetcher, over this system's own network calls.
     * fetcher_init() registered none, because NetSurf was built without
     * curl on purpose. */
    if (fetch_xyuos_register() != NSERROR_OK) {
        printf("netsurf: the network fetcher was refused\n");
        return 1;
    }

    /* And the picture decoders, for the same reason: NetSurf's own want
     * libpng and libjpeg, this system has its own, and nothing has claimed
     * those content types. */
    if (xy_image_init() != NSERROR_OK)
        printf("netsurf: pictures will not be shown\n");

    if (!gui_open(&xy_gui)) {
        printf("netsurf: could not open a window\n");
        return 1;
    }
    xy_clip_reset();

    gui_edit_set(&address, "");
    go_to(argc > 1 ? argv[1] : HOME_PAGE);

    run();

    while (xy_ntabs > 0) xy_tab_close(xy_ntabs - 1);
    /* Before anything else: the kernel must not be left
     * holding a fetch for a program that is about to stop
     * existing. */
    fetch_xyuos_shutdown();

    netsurf_exit();
    nsoption_finalise(nsoptions, nsoptions_default);
    gui_close(&xy_gui);
    return 0;
}
