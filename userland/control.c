/* control -- the control panel.
 *
 * A settings window is only worth having if every control in it changes
 * something immediately and visibly. So this shows exactly the knobs the
 * system actually has, and nothing that would merely look like a setting:
 * pick a theme and the desktop repaints under you, drag the repeat delay and
 * the next key you hold behaves differently.
 *
 * Where a thing cannot be changed it is shown as information rather than
 * dressed up as a control -- the clock is read from the CMOS chip and there is
 * no code here that writes it back, so it is displayed and labelled as such.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"
#include "gui.h"

#define SIDE_W  250
#define HEAD_H  54
#define STAT_H  26

enum { P_APPEARANCE, P_KEYBOARD, P_MOUSE, P_NETWORK, P_SYSTEM, P_POWER, NPANEL };

static const char *panel_name[NPANEL] = {
    "Appearance", "Keyboard", "Mouse", "Network", "System", "Power",
};
static const char *panel_hint[NPANEL] = {
    "Desktop theme",
    "How held keys repeat",
    "Speed, double-click",
    "Address, link, name server",
    "What this machine is",
    "Restart, shut down",
};

static int panel = P_APPEARANCE;
static int hover_side = -1;
static char status[160];

/* --- settings ------------------------------------------------------------- */

static long get(int key)          { return xyuos_syscall3(SYS_SETTING, SETOP_GET, key, 0); }
static long set(int key, long v)  { return xyuos_syscall3(SYS_SETTING, SETOP_SET, key, v); }

static int  nthemes;
static char theme_name[8][32];
static int  cur_theme;
static int  key_delay, key_rate, mouse_spd, dbl_ms;

/* --- network -------------------------------------------------------------
 * Everything here is read back from the stack rather than remembered, because
 * a cable can come out while the window is open. The two buttons block for as
 * long as the network makes them: DHCP can take seconds and a ping waits a
 * second and a half for a reply that may not come. Both paint a line saying
 * what they are doing before they go away and do it. */
static struct net_status ns;
static char net_line[3][80];
static int  net_busy;
static int  nbtn[2][4];            /* button rects, filled by the draw pass */

static void net_refresh(void) { net_status(&ns); }

static void ip4(char *out, int n, unsigned int a) {
    snprintf(out, n, "%u.%u.%u.%u",
             (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
}

static struct si_mem mem;
static char uname_s[64];
static char cpu_s[64];

static void load(void) {
    nthemes = (int)get(SET_THEME_COUNT);
    if (nthemes > 8) nthemes = 8;
    for (int i = 0; i < nthemes; i++)
        xyuos_syscall3(SYS_SETTING, SETOP_THEME_NAME, i, (long)(void *)theme_name[i]);
    cur_theme = (int)get(SET_THEME);
    key_delay = (int)get(SET_KEY_DELAY);
    key_rate  = (int)get(SET_KEY_RATE);
    mouse_spd = (int)get(SET_MOUSE_SPEED);
    dbl_ms    = (int)get(SET_DBLCLICK);

    net_refresh();

    xyuos_sysinfo(SI_MEM, &mem, sizeof mem);
    xyuos_sysinfo(SI_UNAME, uname_s, sizeof uname_s);

    /* The processor's name is the first entry of the device list. */
    static struct si_dev d[4];
    long n = xyuos_sysinfo(SI_DEVICES, d, sizeof d);
    cpu_s[0] = 0;
    if (n > 0) snprintf(cpu_s, sizeof cpu_s, "%s", d[0].name);
}

/* --- layout --------------------------------------------------------------- */

static void side_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int rh = g->fh * 2 + 12;
    *x = 0; *y = HEAD_H + i * rh; *w = SIDE_W; *h = rh;
}

static void body(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = SIDE_W; *y = HEAD_H;
    *w = g->w - SIDE_W; *h = g->h - HEAD_H - STAT_H;
}

/* A labelled slider. Returns the value the pointer is asking for, or the
 * current one when it is not being dragged. */
static int slider(gui_t *g, int x, int y, int w, int val, int lo, int hi,
                  int active, int mx) {
    int th = g->fh;
    gui_panel(g, x, y + th / 3, w, th / 3, GC_PANEL, GC_EDGE);
    int span = hi - lo;
    int v = val < lo ? lo : (val > hi ? hi : val);
    int kx = x + (w - 12) * (v - lo) / (span ? span : 1);
    gui_fill(g, x + 1, y + th / 3 + 1, kx - x, th / 3 - 2, GC_ACCENT);
    gui_panel(g, kx, y, 12, th, active ? GC_BTNDN : GC_BTN, GC_EDGE);
    if (!active) return val;
    int nv = lo + (mx - x - 6) * span / (w - 12 > 0 ? w - 12 : 1);
    if (nv < lo) nv = lo;
    if (nv > hi) nv = hi;
    return nv;
}

/* --- panels --------------------------------------------------------------- */

static int drag_what = -1;      /* which slider the pointer has hold of */

static void draw_appearance(gui_t *g, int x, int y, int w) {
    gui_text(g, x, y, "Desktop theme", GC_TEXT);
    y += g->fh + 10;
    int rh = g->fh + 12;
    for (int i = 0; i < nthemes; i++) {
        int sel = (i == cur_theme);
        int hot = gui_in(g->mx, g->my, x, y, w - 20, rh);
        gui_fill(g, x, y, w - 20, rh, sel ? GC_SEL : (hot ? GC_HOT : GC_PANEL));
        if (sel) gui_fill(g, x, y, 3, rh, GC_ACCENT);
        /* A radio dot, so the list reads as a choice and not as a menu. */
        int cy = y + rh / 2, cx = x + 18;
        gui_rect(g, cx - 5, cy - 5, 10, 10, GC_EDGE);
        if (sel) gui_fill(g, cx - 3, cy - 3, 6, 6, GC_ACCENT);
        gui_text(g, x + 36, y + (rh - g->fh) / 2, theme_name[i], GC_TEXT);
        y += rh + 2;
    }
    y += 8;
    gui_text(g, x, y, "The desktop repaints as soon as you pick one.", GC_DIM);
}

static void draw_keyboard(gui_t *g, int x, int y, int w) {
    char t[96];
    gui_text(g, x, y, "Repeat delay", GC_TEXT);
    y += g->fh + 6;
    snprintf(t, sizeof t, "%d ms before a held key starts repeating", key_delay);
    gui_text(g, x, y, t, GC_DIM);
    y += g->fh + 8;
    key_delay = slider(g, x, y, w - 40, key_delay, 150, 1500, drag_what == 0, g->mx);
    y += g->fh + 22;

    gui_text(g, x, y, "Repeat rate", GC_TEXT);
    y += g->fh + 6;
    snprintf(t, sizeof t, "%d ms between repeats  (about %d a second)",
             key_rate, key_rate ? 1000 / key_rate : 0);
    gui_text(g, x, y, t, GC_DIM);
    y += g->fh + 8;
    key_rate = slider(g, x, y, w - 40, key_rate, 15, 200, drag_what == 1, g->mx);
    y += g->fh + 22;

    gui_text(g, x, y, "Hold a key in any window to feel the change.", GC_DIM);
}

static void draw_mouse(gui_t *g, int x, int y, int w) {
    char t[96];
    gui_text(g, x, y, "Pointer speed", GC_TEXT);
    y += g->fh + 6;
    snprintf(t, sizeof t, "%d%% of the hardware's own scale", mouse_spd);
    gui_text(g, x, y, t, GC_DIM);
    y += g->fh + 8;
    mouse_spd = slider(g, x, y, w - 40, mouse_spd, 25, 400, drag_what == 2, g->mx);
    y += g->fh + 22;

    gui_text(g, x, y, "Double-click speed", GC_TEXT);
    y += g->fh + 6;
    snprintf(t, sizeof t, "two clicks within %d ms count as one double click", dbl_ms);
    gui_text(g, x, y, t, GC_DIM);
    y += g->fh + 8;
    dbl_ms = slider(g, x, y, w - 40, dbl_ms, 150, 1000, drag_what == 3, g->mx);
    y += g->fh + 22;

    gui_text(g, x, y, "Try it on a window's title bar: a double click", GC_DIM);
    y += g->fh + 2;
    gui_text(g, x, y, "maximises it.", GC_DIM);
}

static const char *net_btn_label[2] = { "Connect", "Test" };

static void draw_network(gui_t *g, int x, int y, int w) {
    char t[128], v[64];
    int lh = g->fh + 8, y0 = y;
    (void)w;

    if (!ns.driver[0]) {
        gui_text(g, x, y, "No network card found.", GC_TEXT);
        y += lh + 4;
        gui_text(g, x, y, "Nothing here can be configured until a driver binds one.", GC_DIM);
        nbtn[0][2] = nbtn[1][2] = 0;       /* no buttons -> nothing to hit */
        return;
    }

    snprintf(t, sizeof t, "Adapter:  %s", ns.driver);
    gui_text(g, x, y, t, GC_TEXT); y += lh;

    snprintf(t, sizeof t, "Hardware address:  %02x:%02x:%02x:%02x:%02x:%02x",
             ns.mac[0], ns.mac[1], ns.mac[2], ns.mac[3], ns.mac[4], ns.mac[5]);
    gui_text(g, x, y, t, GC_DIM); y += lh;

    gui_text(g, x, y, "Link:", GC_DIM);
    gui_text(g, x + g->fw * 7, y, ns.link ? "connected" : "no carrier",
             ns.link ? GC_GOOD : GC_WARN);
    y += lh + 10;

    if (ns.up) {
        static const char *lab[4] = { "Address", "Netmask", "Gateway", "Name server" };
        unsigned int val[4];
        val[0] = ns.ip; val[1] = ns.mask; val[2] = ns.gw; val[3] = ns.dns;
        for (int i = 0; i < 4; i++) {
            ip4(v, sizeof v, val[i]);
            snprintf(t, sizeof t, "%-13s %s", lab[i], v);
            gui_text(g, x, y, t, GC_TEXT); y += lh;
        }
        y += 4;
        gui_text(g, x, y, "Leased over DHCP.", GC_DIM);
    } else {
        gui_text(g, x, y, "No address.", GC_TEXT); y += lh;
        gui_text(g, x, y, ns.link ? "Connect asks a DHCP server for one."
                                  : "Plug a cable in first, then Connect.", GC_DIM);
    }

    /* The button row sits at a fixed height rather than under whatever the
     * block above happened to be, so that pressing Connect -- which makes that
     * block four lines taller -- does not move Test out from under the
     * pointer that is about to press it. */
    y = y0 + lh * 9 + 20;

    for (int i = 0; i < 2; i++) {
        int bw = g->fw * 12, bh = g->fh + 16;
        int bx = x + i * (bw + 12), by = y;
        nbtn[i][0] = bx; nbtn[i][1] = by; nbtn[i][2] = bw; nbtn[i][3] = bh;
        gui_button(g, bx, by, bw, bh, net_btn_label[i],
                   net_busy ? GB_OFF :
                   (gui_in(g->mx, g->my, bx, by, bw, bh) ? GB_HOVER : GB_NORMAL));
    }
    y += g->fh + 16 + 14;

    for (int i = 0; i < 3; i++)
        if (net_line[i][0]) { gui_text(g, x, y, net_line[i], GC_DIM); y += lh; }
}

static void draw_system(gui_t *g, int x, int y, int w) {
    char t[128];
    (void)w;
    int lh = g->fh + 8;

    gui_text(g, x, y, uname_s[0] ? uname_s : "xyuOS Neo", GC_TEXT);
    y += lh + 4;

    snprintf(t, sizeof t, "Processor:  %s", cpu_s[0] ? cpu_s : "unknown");
    gui_text(g, x, y, t, GC_DIM); y += lh;

    if (mem.total_frames) {
        unsigned long total = (unsigned long)(mem.total_frames * mem.page_size / (1024 * 1024));
        unsigned long freem = (unsigned long)(mem.free_frames  * mem.page_size / (1024 * 1024));
        snprintf(t, sizeof t, "Memory:  %lu MB total, %lu MB free", total, freem);
        gui_text(g, x, y, t, GC_DIM); y += lh;
    }

    unsigned up = uptime_ms() / 1000;
    snprintf(t, sizeof t, "Uptime:  %uh %02um %02us", up / 3600, (up / 60) % 60, up % 60);
    gui_text(g, x, y, t, GC_DIM); y += lh;

    struct xyuos_tm tm;
    xyuos_time(&tm);
    {
        snprintf(t, sizeof t, "Clock:  %04d-%02d-%02d  %02d:%02d:%02d",
                 tm.year, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
        gui_text(g, x, y, t, GC_DIM); y += lh;
        gui_text(g, x, y, "(read from the CMOS clock; setting it is not wired up yet)",
                 GC_DIM);
        y += lh;
    }

    y += 8;
    gui_text(g, x, y, "Device manager lists everything in the machine.", GC_DIM);
}

static const char *power_label[3] = { "Restart", "Shut down", "Sleep" };

static void power_rect(gui_t *g, int i, int x, int y, int *bx, int *by, int *bw, int *bh) {
    *bw = g->fw * 14; *bh = g->fh + 16;
    *bx = x; *by = y + i * (*bh + 10);
}

static void draw_power(gui_t *g, int x, int y, int w) {
    (void)w;
    gui_text(g, x, y, "These act immediately.", GC_DIM);
    y += g->fh + 14;
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh;
        power_rect(g, i, x, y, &bx, &by, &bw, &bh);
        gui_button(g, bx, by, bw, bh, power_label[i],
                   gui_in(g->mx, g->my, bx, by, bw, bh) ? GB_HOVER : GB_NORMAL);
    }
}

/* --- frame ---------------------------------------------------------------- */

static void draw(gui_t *g) {
    gui_clear(g, GC_WIN);

    /* header */
    gui_vgrad(g, 0, 0, g->w, HEAD_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, HEAD_H - 1, g->w, 1, GC_EDGE);
    gui_text_s(g, 16, (HEAD_H - g->fh * 2) / 2, "Control panel", GC_TEXT, 2);

    /* the category list down the left */
    gui_fill(g, 0, HEAD_H, SIDE_W, g->h - HEAD_H - STAT_H, GC_PANEL);
    gui_fill(g, SIDE_W - 1, HEAD_H, 1, g->h - HEAD_H - STAT_H, GC_EDGE);
    for (int i = 0; i < NPANEL; i++) {
        int x, y, w, h;
        side_rect(g, i, &x, &y, &w, &h);
        unsigned bg = (i == panel) ? GC_SEL : (i == hover_side ? GC_HOT : GC_PANEL);
        gui_fill(g, x, y, w, h, bg);
        if (i == panel) gui_fill(g, x, y, 3, h, GC_ACCENT);
        gui_text(g, x + 14, y + 6, panel_name[i], GC_TEXT);
        gui_text_clip(g, x + 14, y + 6 + g->fh, panel_hint[i], GC_DIM, w - 20);
    }

    int bx, by, bw, bh;
    body(g, &bx, &by, &bw, &bh);
    gui_fill(g, bx, by, bw, bh, GC_WIN);
    int cx = bx + 22, cy = by + 20;
    switch (panel) {
        case P_APPEARANCE: draw_appearance(g, cx, cy, bw - 40); break;
        case P_KEYBOARD:   draw_keyboard(g, cx, cy, bw - 40); break;
        case P_MOUSE:      draw_mouse(g, cx, cy, bw - 40); break;
        case P_NETWORK:    draw_network(g, cx, cy, bw - 40); break;
        case P_SYSTEM:     draw_system(g, cx, cy, bw - 40); break;
        case P_POWER:      draw_power(g, cx, cy, bw - 40); break;
    }

    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    gui_text_clip(g, 10, g->h - STAT_H + (STAT_H - g->fh) / 2,
                  status[0] ? status : "Changes take effect at once.",
                  GC_TEXT, g->w - 20);
}

/* The two network buttons take their time, so each one paints the window
 * saying so before it blocks -- otherwise the desktop looks frozen for the
 * seconds DHCP spends waiting for an answer that may never come. */
static void net_say(gui_t *g, const char *l0, const char *l1, const char *l2) {
    snprintf(net_line[0], sizeof net_line[0], "%s", l0 ? l0 : "");
    snprintf(net_line[1], sizeof net_line[1], "%s", l1 ? l1 : "");
    snprintf(net_line[2], sizeof net_line[2], "%s", l2 ? l2 : "");
    if (gui_sync(g)) { draw(g); gui_present(g); }
}

static void net_connect(gui_t *g) {
    net_busy = 1;
    net_say(g, "Asking for an address...", 0, 0);
    int ok = net_up();
    net_refresh();
    net_busy = 0;
    if (ok && ns.up) {
        char v[64]; ip4(v, sizeof v, ns.ip);
        snprintf(net_line[0], sizeof net_line[0], "Got %s from DHCP.", v);
        net_line[1][0] = net_line[2][0] = 0;
    } else {
        net_say(g, ns.link ? "No DHCP server answered."
                           : "No carrier -- is the cable in?", 0, 0);
    }
}

static void net_test(gui_t *g) {
    char v[64];
    net_busy = 1;
    net_say(g, "Testing...", 0, 0);

    if (!ns.up && !net_up()) { net_refresh(); net_busy = 0;
        net_say(g, "No address, so there is nothing to test.", 0, 0); return; }
    net_refresh();

    /* Three questions, in the order the answers stop being useful: can we
     * reach the next hop, can we turn a name into an address, and can we
     * reach that address. A failure at any step explains the ones after it. */
    ip4(v, sizeof v, ns.gw);
    int rtt = net_ping(v);
    if (rtt >= 0) {
        if (rtt >= 1000) snprintf(net_line[0], sizeof net_line[0],
                                  "Gateway %s replied in %u.%u ms", v,
                                  (unsigned)rtt / 1000u, ((unsigned)rtt % 1000u) / 100u);
        else             snprintf(net_line[0], sizeof net_line[0],
                                  "Gateway %s replied in %u us", v, (unsigned)rtt);
    } else {
        snprintf(net_line[0], sizeof net_line[0], "Gateway %s did not reply", v);
    }

    unsigned int ip = 0;
    if (net_resolve("example.com", &ip) == 0 && ip) {
        ip4(v, sizeof v, ip);
        snprintf(net_line[1], sizeof net_line[1], "example.com resolves to %s", v);
        int r2 = net_ping("example.com");
        if (r2 >= 0) snprintf(net_line[2], sizeof net_line[2],
                              "and answers in %u.%u ms",
                              (unsigned)r2 / 1000u, ((unsigned)r2 % 1000u) / 100u);
        else snprintf(net_line[2], sizeof net_line[2],
                      "but does not answer a ping (many hosts do not)");
    } else {
        snprintf(net_line[1], sizeof net_line[1], "Name lookup failed -- DNS is not answering");
        net_line[2][0] = 0;
    }
    net_busy = 0;
}

/* --- input ---------------------------------------------------------------- */

/* Which slider, if any, is under the pointer on this panel. Kept beside the
 * drawing code because the two have to agree about the geometry. */
static int slider_hit(gui_t *g, int mx, int my) {
    int bx, by, bw, bh;
    body(g, &bx, &by, &bw, &bh);
    int x = bx + 22, w = bw - 40;
    int y = by + 20;
    int rows[4], n = 0;
    if (panel == P_KEYBOARD) {
        rows[n++] = y + (g->fh + 6) + (g->fh + 8);
        rows[n++] = rows[0] + (g->fh + 22) + (g->fh + 6) + (g->fh + 8);
    } else if (panel == P_MOUSE) {
        rows[n++] = y + (g->fh + 6) + (g->fh + 8);
        rows[n++] = rows[0] + (g->fh + 22) + (g->fh + 6) + (g->fh + 8);
    } else {
        return -1;
    }
    for (int i = 0; i < n; i++)
        if (gui_in(mx, my, x, rows[i] - 4, w - 40, g->fh + 8))
            return (panel == P_KEYBOARD ? 0 : 2) + i;
    return -1;
}

static void apply_sliders(void) {
    set(SET_KEY_DELAY, key_delay);
    set(SET_KEY_RATE, key_rate);
    set(SET_MOUSE_SPEED, mouse_spd);
    set(SET_DBLCLICK, dbl_ms);
}

int main(void) {
    gui_t g;
    if (!gui_open(&g)) return 1;
    load();

    int running = 1, dirty = 1;

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }
                if (k->code == XKEY_ESC) running = 0;
                else if (k->code == XKEY_UP   && panel > 0) { panel--; if (panel == P_NETWORK) net_refresh(); }
                else if (k->code == XKEY_DOWN && panel < NPANEL - 1) { panel++; if (panel == P_NETWORK) net_refresh(); }
                continue;
            }

            mouse_event_t *m = &e.m;
            int was = hover_side;
            hover_side = -1;
            for (int i = 0; i < NPANEL; i++) {
                int x, y, w, h;
                side_rect(&g, i, &x, &y, &w, &h);
                if (gui_in(m->x, m->y, x, y, w, h)) hover_side = i;
            }
            if (hover_side != was || gui_acts(&e) || drag_what >= 0) dirty = 1;

            if (m->pressed & MB_LEFT) {
                if (hover_side >= 0) {
                    panel = hover_side; status[0] = 0; drag_what = -1;
                    if (panel == P_NETWORK) net_refresh();
                }
                else {
                    drag_what = slider_hit(&g, m->x, m->y);

                    int bx, by, bw, bh;
                    body(&g, &bx, &by, &bw, &bh);
                    int cx = bx + 22, cy = by + 20;

                    if (panel == P_APPEARANCE) {
                        int rh = g.fh + 12, y = cy + g.fh + 10;
                        for (int i = 0; i < nthemes; i++, y += rh + 2) {
                            if (!gui_in(m->x, m->y, cx, y, bw - 60, rh)) continue;
                            cur_theme = i;
                            set(SET_THEME, i);
                            snprintf(status, sizeof status, "theme: %s", theme_name[i]);
                        }
                    } else if (panel == P_NETWORK) {
                        for (int i = 0; i < 2 && !net_busy; i++) {
                            if (!nbtn[i][2]) continue;
                            if (!gui_in(m->x, m->y, nbtn[i][0], nbtn[i][1],
                                        nbtn[i][2], nbtn[i][3])) continue;
                            if (i == 0) net_connect(&g); else net_test(&g);
                            dirty = 1;
                        }
                    } else if (panel == P_POWER) {
                        int y = cy + g.fh + 14;
                        for (int i = 0; i < 3; i++) {
                            int px, py, pw, ph;
                            power_rect(&g, i, cx, y, &px, &py, &pw, &ph);
                            if (!gui_in(m->x, m->y, px, py, pw, ph)) continue;
                            gui_close(&g);
                            xyuos_power(i == 0 ? XYUOS_REBOOT :
                                        i == 1 ? XYUOS_OFF : XYUOS_SLEEP);
                            /* Sleep returns; the other two do not. */
                            gui_open(&g);
                            load();
                        }
                    }
                }
            }
            if (m->released & MB_LEFT) {
                if (drag_what >= 0) { apply_sliders(); status[0] = 0; }
                drag_what = -1;
            }
            if (drag_what >= 0 && (m->buttons & MB_LEFT)) {
                /* The draw pass reads the pointer position out of g, so a
                 * redraw is all that is needed to follow the drag. */
                dirty = 1;
            }
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g);
                gui_present(&g);
                if (drag_what >= 0) apply_sliders();
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
