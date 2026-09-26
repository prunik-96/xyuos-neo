#include "wm.h"
#include "pane.h"
#include "../kernel/process.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../drivers/rtc.h"
#include "../arch/x86_64/pit.h"
#include "../gfx/font.h"
#include "../gfx/icons.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../arch/x86_64/smp.h"
#include "../fs/vfs.h"
#include "../fs/fat32.h"
#include "../kernel/kio.h"
#include "../drivers/mouse.h"
#include "../drivers/audio.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define MAX_PANES 16
#define MAX_NODES 40

// Cell size comes from the anti-aliased font at runtime.
#define GW (font_cell_w())
#define GH (font_cell_h())

// Modern dark theme (Tokyo Night-ish). Soft background, low-contrast chrome,
// an accent-blue focus border, and a few px of inner padding so text breathes.
#define BORDER              2      // accent border thickness
#define PAD                 6      // inner padding between border and text
#define INSET               (BORDER + PAD)
#define GAP                 10     // desktop gap around/between panes (i3-gaps)
#define BAR_H               24     // top status bar height
#define COLOR_BG            0x001A1B26   // pane background
#define COLOR_FG            0x00C0CAF5   // default text
#define COLOR_DESKTOP       0x0016161E   // fallback desktop (if no wallpaper)
#define COLOR_BORDER_FOCUS  0x007AA2F7   // accent blue
#define COLOR_BORDER_NORMAL 0x00292E42   // subtle
#define COLOR_CURSOR        0x007AA2F7
#define COLOR_BAR           0x0014151F   // status bar strip
#define COLOR_BAR_FG        0x007AA2F7   // status bar accent text
#define COLOR_BAR_DIM       0x00565F89   // status bar muted text

// ==========================================================================
//  Aero themes (Windows 7-ish glass).  A tiling compositor wearing Aero
//  chrome: glassy window title bars, a bottom taskbar with a Start orb, a
//  live "gadget", and a flowing wallpaper. Two variants -- dark and light.
// ==========================================================================
#define TASKBAR_H  46      // bottom taskbar height
#define TITLE_H    26      // per-window glass title bar
#define FRAME      3       // window glass edge thickness
#define EDGE       2       // the glow frame actually drawn at that edge
#define TPAD       6       // inner padding inside the content area
#define AGAP       8       // gap of wallpaper between windows

typedef struct {
    const char *name;
    uint32_t wp_a, wp_b, wp_glow;      // wallpaper gradient + glow
    uint32_t title_a, title_b, title_hl;  // title bar gradient + top highlight
    uint32_t glow, glow_dim;           // focused / unfocused window edge
    uint32_t content_bg;               // terminal background
    uint32_t bar_a, bar_b;             // taskbar gradient
    uint32_t menu_bg;                  // start menu / Alt+Tab panel tint
    uint32_t orb;                      // start orb accent
    uint32_t title_text, title_text_dim;
    uint32_t bar_text, bar_dim;

    // What a PROGRAM draws with. Until now a theme only reached the window
    // frame, so a black desktop came with a white task manager inside it --
    // the chrome was themed and the contents were not. These are the same
    // sixteen roles gui.h has always had, moved out of the toolkit and into
    // the theme so they can differ between the two.
    //
    // The terminal is deliberately NOT here: content_bg above stays dark in
    // both themes, because a white console is nobody's idea of a light theme.
    uint32_t ui_win, ui_panel, ui_alt, ui_text, ui_dim, ui_line, ui_edge;
    uint32_t ui_accent, ui_accent2, ui_sel, ui_hot, ui_btn, ui_btndn;
    uint32_t ui_bar, ui_bar2, ui_warn, ui_good;
} theme_t;

static const theme_t THEMES[] = {
    {   // 0: Black Aero -- dark smoked glass
        .name = "Black Aero",
        .wp_a = 0x00121722, .wp_b = 0x00050608, .wp_glow = 0x00204A7A,
        .title_a = 0x00343A44, .title_b = 0x00161A22, .title_hl = 0x00505864,
        .glow = 0x003E9BE0, .glow_dim = 0x00303640,
        .content_bg = 0x000B0D12,
        .bar_a = 0x002A303C, .bar_b = 0x000C0E14,
        .menu_bg = 0x00101820,
        .orb = 0x003E9BE0,
        .title_text = 0x00E6ECF5, .title_text_dim = 0x008A93A2,
        .bar_text = 0x00E6ECF5, .bar_dim = 0x008A93A2,
        .ui_win = 0x00171A21, .ui_panel = 0x0011141A, .ui_alt = 0x00151920,
        .ui_text = 0x00E6ECF5, .ui_dim = 0x008A93A2,
        .ui_line = 0x00252A33, .ui_edge = 0x0039414E,
        .ui_accent = 0x003E9BE0, .ui_accent2 = 0x007CC4F0,
        .ui_sel = 0x001E3A56, .ui_hot = 0x001A2430,
        .ui_btn = 0x00232935, .ui_btndn = 0x00171C25,
        .ui_bar = 0x001C212B, .ui_bar2 = 0x00141922,
        .ui_warn = 0x00E0644A, .ui_good = 0x005FBF7F,
    },
    {   // 1: Aero -- light blue glass (classic Win7)
        .name = "Aero",
        .wp_a = 0x00246FB0, .wp_b = 0x00113A63, .wp_glow = 0x0066C2E0,
        .title_a = 0x00EAF4FE, .title_b = 0x00B6D6F2, .title_hl = 0x00FFFFFF,
        .glow = 0x005AA0E0, .glow_dim = 0x008FB0D0,
        .content_bg = 0x00101820,
        .bar_a = 0x00CFE3F7, .bar_b = 0x007FA9D6,
        .menu_bg = 0x00DCE8F5,
        .orb = 0x003E86C8,
        .title_text = 0x00123048, .title_text_dim = 0x004A6A88,
        .bar_text = 0x00133154, .bar_dim = 0x00436486,
        // The light set is what gui.h always shipped, so this theme looks
        // exactly as it did.
        .ui_win = 0x00F2F3F5, .ui_panel = 0x00FFFFFF, .ui_alt = 0x00F7F9FB,
        .ui_text = 0x001B1B1F, .ui_dim = 0x006B7280,
        .ui_line = 0x00D5D9DE, .ui_edge = 0x00B9C0C8,
        .ui_accent = 0x002A6FD6, .ui_accent2 = 0x001B4F9C,
        .ui_sel = 0x00CFE3FB, .ui_hot = 0x00E6F0FB,
        .ui_btn = 0x00ECEEF1, .ui_btndn = 0x00D3DAE2,
        .ui_bar = 0x00E9ECEF, .ui_bar2 = 0x00DDE2E7,
        .ui_warn = 0x00C24A2F, .ui_good = 0x002E7D46,
    },
};
#define NTHEMES (int)(sizeof(THEMES) / sizeof(THEMES[0]))

#define SPLIT_H 0   /* children side by side (left|right), divides width  */
#define SPLIT_V 1   /* children stacked (top|bottom), divides height      */

// 8-color palette (indices match pane.h PC_*). Index 0 ("black") MUST equal
// COLOR_BG so default-bg cells blend seamlessly with the interior fill.
static const uint32_t pane_palette[8] = {
    0x001A1B26, // black  = background
    0x00F7768E, // red
    0x009ECE6A, // green
    0x00E0AF68, // yellow
    0x007AA2F7, // blue
    0x00BB9AF7, // magenta
    0x007DCFFF, // cyan
    0x00C0CAF5, // white / default fg
};

// A floating window: a pane, where it sits, how it stacks, and which workspace
// it belongs to. Windows overlap freely -- there is no tiling tree any more,
// and `z` alone decides what is in front.
#define WIN_NORMAL 0
#define WIN_MAX    1
#define WIN_MIN    2

struct wm_node {
    int used;
    int pane_idx;                // index into panes[]
    int z;                       // stacking order; larger is nearer the viewer
    int ws;                      // workspace this window lives on
    int state;                   // WIN_NORMAL / WIN_MAX / WIN_MIN

    uint32_t x, y, w, h;         // frame rectangle on screen
    uint32_t sx, sy, sw, sh;     // geometry remembered across a maximise
};

static struct pane    panes[MAX_PANES];
static struct wm_node nodes[MAX_NODES];
static int focused = -1;   // node index of the focused window
static int z_top = 0;      // highest stacking order handed out so far
static int split_request = 0;  // set by the shell's `hyper` command
static int started = 0;    // wm_start() has run; wm_poll() is meaningful
static int dirty = 0;      // something changed; repaint on the next poll
static int theme = 0;      // index into THEMES[] (0 = Black Aero)
static int dblclick_ms = 400;   // how close two clicks must be to be a double
#define TH (&THEMES[theme])

// --- virtual workspaces ----------------------------------------------------
// Each workspace is an independent tiling tree; switching simply swaps which
// (root, focused) pair is live. Panes on a hidden workspace are not rendered,
// but their processes keep running and keep their panes -- wm_pane_for_pid
// scans the pane pool rather than the tree -- so a build or a download carries
// on in the background exactly as it would on a real desktop.
#define MAX_WS 9
static int ws_focused[MAX_WS];
static int cur_ws = 0;

// System Monitor gadget: hidden by default, toggled with Super+M. Keeps a
// one-per-second history of CPU and memory load for its little graphs.
static int monitor_on = 0;

// The desktop background only becomes visible again when window geometry
// changes, so it is repainted on demand rather than every frame: the back
// buffer keeps last frame's pixels, and panes/taskbar fully repaint their own
// rectangles. Raised by relayout(), a theme change, and hiding the gadget.
static int wp_dirty = 1;
#define HIST 60
#define UI_CORES 16                       // per-core graphs the gadget can show
static uint8_t cpu_hist[HIST], mem_hist[HIST];
static uint8_t core_hist[UI_CORES][HIST]; // one load history per core
static int     hist_pos = 0, hist_count = 0;
static uint8_t cur_cpu = 0, cur_mem = 0;
// Frames that reached the screen in the last second. Counted from
// fb_frames_pushed(), which only counts presents that pushed real pixels --
// so this is what the display got, not what the compositor was asked for.
static uint16_t cur_fps = 0;

// Called from the shell engine (`hyper` command) to tile the current pane.
void wm_request_split(void) {
    split_request = 1;
}

static void spawn_shell_in(int pane_idx);
static void spawn_prog_in(int pane_idx, const char *path, const char *arg);
static void pane_push_event(struct pane *p, const struct kbd_event *ev);
static void refresh_leaves(void);
static int  collect_windows(int ws, int *out, int max);
static int  topmost_window(int ws);
static int  ws_has_windows(int ws);

// --- pool allocation ---

// Claiming a slot has to be one indivisible step.
//
// Windows used to be created only from the idle path, one at a time. Now a
// program can ask for one through a syscall, and a syscall can be preempted
// by the timer -- so two callers can be between "found a free slot" and "took
// it" at the same moment and walk away with the same one. Interrupts off for
// the handful of instructions that matter is the whole fix on a single core.
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}

static int alloc_pane(void) {
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_PANES; i++) {
        if (!panes[i].alive) {
            panes[i].alive = 1;          // claim it here, not in pane_init
            panes[i].owner_pid = 0;
            panes[i].app_pane = 0;
            panes[i].keys_raw = 0;
            panes[i].keys_raw_pid = 0;
            irq_restore(f);
            return i;
        }
    }
    irq_restore(f);
    return -1;
}

static int alloc_node(void) {
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used) {
            nodes[i].used = 1;
            nodes[i].z = 0;
            nodes[i].state = WIN_NORMAL;
            irq_restore(f);
            return i;
        }
    }
    irq_restore(f);
    return -1;
}

static void free_node(int n) {
    if (n >= 0) nodes[n].used = 0;
}

// --- geometry ---

static uint32_t icols(uint32_t w) {
    if (w < 2 * INSET + GW) return 1;
    return (w - 2 * INSET) / GW;
}
static uint32_t irows(uint32_t h) {
    if (h < 2 * INSET + GH) return 1;
    return (h - 2 * INSET) / GH;
}

// The content (terminal / gfx) rectangle inside a node's window chrome:
// below the glass title bar, inside the frame + inner padding.
// Where a window's content goes.
//
// A terminal wants a margin -- a grid of characters pressed against the frame
// looks cramped -- and that margin is painted in the terminal's own background
// colour, which stays dark in both themes on purpose. A program drawing its
// own interface wants no margin at all, and got one anyway: the dark line down
// the side of every window in the light theme was not a border, it was seven
// pixels of console showing past the edge of the program.
//
// So the padding belongs to the terminal, and a pane painting its own pixels
// gets the whole interior, right up to the glow frame.
static void content_rect(int n, uint32_t *cx, uint32_t *cy, uint32_t *cw, uint32_t *ch) {
    struct wm_node *nd = &nodes[n];
    int gfx = nd->pane_idx >= 0 && panes[nd->pane_idx].gfx_on;
    uint32_t lr = gfx ? EDGE : FRAME + TPAD, top = TITLE_H;
    uint32_t bot = gfx ? EDGE : FRAME + TPAD;
    *cx = nd->x + lr;
    *cy = nd->y + top;
    *cw = (nd->w > 2 * lr) ? nd->w - 2 * lr : 0;
    *ch = (nd->h > top + bot) ? nd->h - top - bot : 0;
}

// The desktop rectangle windows live in: everything above the taskbar.
static void desk_area(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t W = fb_get_width(), H = fb_get_height();
    *x = AGAP;
    *y = AGAP;
    *w = (W > 2 * AGAP) ? W - 2 * AGAP : W;
    *h = (H > TASKBAR_H + 2 * AGAP) ? H - TASKBAR_H - 2 * AGAP : H;
}

static uint32_t win_min_w(void) { return 2 * (FRAME + TPAD) + 24 * GW; }
static uint32_t win_min_h(void) { return TITLE_H + FRAME + 6 * GH; }

// Settle one window: apply the maximised rectangle, keep it inside the desktop,
// and resize its character grid to match the new interior.
static void layout_window(int n) {
    struct wm_node *nd = &nodes[n];
    uint32_t dx, dy, dw, dh;
    desk_area(&dx, &dy, &dw, &dh);

    if (nd->state == WIN_MAX) {
        nd->x = dx; nd->y = dy; nd->w = dw; nd->h = dh;
    } else {
        uint32_t mw = win_min_w(), mh = win_min_h();
        if (nd->w < mw) nd->w = mw;
        if (nd->h < mh) nd->h = mh;
        if (nd->w > dw) nd->w = dw;
        if (nd->h > dh) nd->h = dh;
        // A window may never leave the desktop, or its title bar (the only way
        // to drag it back) would be unreachable.
        if (nd->x < dx) nd->x = dx;
        if (nd->y < dy) nd->y = dy;
        if (nd->x + nd->w > dx + dw) nd->x = dx + dw - nd->w;
        if (nd->y + nd->h > dy + dh) nd->y = dy + dh - nd->h;
    }

    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    uint32_t cols = cw / GW, rows = ch / GH;
    pane_resize(&panes[nd->pane_idx], cols ? cols : 1, rows ? rows : 1);
}

static void relayout(void) {
    wp_dirty = 1;              // window geometry moved: the desktop shows through
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].ws == cur_ws && nodes[i].state != WIN_MIN)
            layout_window(i);
}

// Aero-style snap: while a window is dragged against a screen edge we show
// where it would land, and commit it on release.
#define SNAP_NONE 0
#define SNAP_L    1
#define SNAP_R    2
#define SNAP_MAX  3
#define SNAP_ZONE 14
static int snap_hint = SNAP_NONE;

// Where the previous click landed, for double-click detection.
static uint64_t last_click_ms = 0;
static int last_click_win = -1;

// The rectangle a snap hint would put a window in.
static void snap_rect(int hint, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t dx, dy, dw, dh;
    desk_area(&dx, &dy, &dw, &dh);
    *x = dx; *y = dy; *w = dw; *h = dh;
    if (hint == SNAP_L) { *w = dw / 2; }
    else if (hint == SNAP_R) { *w = dw / 2; *x = dx + dw - *w; }
}

// Which edge the pointer is against, if any.
static int snap_from_pointer(int mx, int my) {
    uint32_t dx, dy, dw, dh;
    desk_area(&dx, &dy, &dw, &dh);
    if (my <= (int)dy + SNAP_ZONE) return SNAP_MAX;
    if (mx <= (int)dx + SNAP_ZONE) return SNAP_L;
    if (mx >= (int)(dx + dw) - SNAP_ZONE) return SNAP_R;
    return SNAP_NONE;
}

// Bring a window to the front of the stack.
static void raise_window(int n) {
    if (n < 0 || !nodes[n].used) return;
    nodes[n].z = ++z_top;
}

// --- rendering ---

// Nearest-neighbour blit of an ARGB source (sw x sh) into the framebuffer
// rectangle (dx,dy,dw,dh). Used for graphics-mode panes (DOOM).
static void blit_scaled(uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh,
                        const uint32_t *src, uint32_t sw, uint32_t sh) {
    if (!sw || !sh || !dw || !dh) return;
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    uint32_t fbw = fb_get_width(), fbh = fb_get_height();
    fb_mark_rect(dx, dy, dw, dh);

    // A program sizes its surface to the window interior, so this is normally
    // one-to-one -- worth its own loop, because the general path pays a
    // multiply and a divide for every pixel just to compute an index that
    // turns out to be the one next door.
    if (sw == dw && sh == dh) {
        for (uint32_t j = 0; j < dh; j++) {
            uint32_t py = dy + j;
            if (py >= fbh) break;
            const uint32_t *srow = src + (size_t)j * sw;
            uint32_t *drow = (uint32_t *)(base + py * pitch) + dx;
            uint32_t n = dw;
            if (dx + n > fbw) n = (dx < fbw) ? fbw - dx : 0;
            // Two pixels per store. Without SSE the compiler will not widen a
            // 32-bit copy loop by itself, and this is the hottest loop on the
            // desktop: every window's content passes through it.
            uint32_t i = 0;
            if (((uintptr_t)drow & 7) && n) { drow[0] = srow[0]; i = 1; }
            uint32_t pairs = (n - i) / 2;
            const uint64_t *s8 = (const uint64_t *)(srow + i);
            uint64_t *d8 = (uint64_t *)(drow + i);
            for (uint32_t k = 0; k < pairs; k++) d8[k] = s8[k];
            for (uint32_t t = i + pairs * 2; t < n; t++) drow[t] = srow[t];
        }
        return;
    }

    for (uint32_t j = 0; j < dh; j++) {
        uint32_t py = dy + j;
        if (py >= fbh) break;
        uint32_t sy = (j * sh) / dh;
        const uint32_t *srow = src + sy * sw;
        volatile uint32_t *drow = (volatile uint32_t *)(base + py * pitch);
        for (uint32_t i = 0; i < dw; i++) {
            uint32_t px = dx + i;
            if (px >= fbw) break;
            drow[px] = srow[(i * sw) / dw];
        }
    }
}

// --- little drawing helpers (gradients, transparent text, glow, corners) --
static inline uint32_t mix(uint32_t a, uint32_t b, int t) {   // t: 0..256
    int ar=(a>>16)&255, ag=(a>>8)&255, ab=a&255;
    int br=(b>>16)&255, bg=(b>>8)&255, bb=b&255;
    int r=(ar*(256-t)+br*t)>>8, g=(ag*(256-t)+bg*t)>>8, bl=(ab*(256-t)+bb*t)>>8;
    return ((uint32_t)r<<16)|((uint32_t)g<<8)|bl;
}

static void fill_vgrad(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t c0, uint32_t c1) {
    if (!w || !h) return;
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch(), fbw = fb_get_width(), fbh = fb_get_height();
    for (uint32_t j = 0; j < h; j++) {
        uint32_t py = y + j; if (py >= fbh) break;
        uint32_t col = mix(c0, c1, (int)(j * 256 / h));
        volatile uint32_t *row = (volatile uint32_t *)(base + py * pitch);
        for (uint32_t i = 0; i < w; i++) { uint32_t px = x + i; if (px >= fbw) break; row[px] = col; }
    }
}

/* Strings are UTF-8, and one character is one cell wide however many bytes
 * it took to write it. */
static void draw_text_t(int x, int y, const char *s, uint32_t fg) {
    int col = 0;
    for (int i = 0; s[i]; ) {
        int cp, n = utf8_decode(s + i, 4, &cp);
        i += n;
        font_draw_glyph_t(x + col * (int)GW, y, cp, fg);
        col++;
    }
}


// Wallpaper cache (defined below); used to restore rounded corners.
static uint32_t *wallpaper;
static uint32_t  wp_w, wp_h;

static void round_corners(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int r) {
    if (!wallpaper || w < (uint32_t)2*r || h < (uint32_t)2*r) return;
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (int cy = 0; cy < r; cy++) {
        for (int cx = 0; cx < r; cx++) {
            int dx = r - cx, dy = r - cy;
            if (dx*dx + dy*dy <= r*r) continue;        // inside the arc: keep
            // four corners -> restore the wallpaper pixel underneath
            uint32_t pts[4][2] = {
                { x + cx,          y + cy },
                { x + w - 1 - cx,  y + cy },
                { x + cx,          y + h - 1 - cy },
                { x + w - 1 - cx,  y + h - 1 - cy },
            };
            for (int k = 0; k < 4; k++) {
                uint32_t px = pts[k][0], py = pts[k][1];
                if (px >= wp_w || py >= wp_h) continue;
                *(volatile uint32_t *)(base + py*pitch + px*4) = wallpaper[py*wp_w + px];
            }
        }
    }
}

static const char *pane_title(struct pane *p) {
    if (p->owner_pid <= 0) return "desktop";
    int fg = process_foreground(p->owner_pid);
    if (fg <= 0) fg = p->owner_pid;
    static struct si_proc procs[24];
    int nn = process_list(procs, 24);
    for (int i = 0; i < nn; i++)
        if (procs[i].pid == fg && procs[i].name[0]) return procs[i].name;
    return "shell";
}

// Draw the terminal grid / gfx surface into a content rectangle.
static void draw_content(int n, uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch, int focus) {
    struct pane *p = &panes[nodes[n].pane_idx];
    if (p->gfx_on && p->gfx) { blit_scaled(cx, cy, cw, ch, p->gfx, p->gfx_w, p->gfx_h); return; }
    fb_fill_rect(cx, cy, cw, ch, TH->content_bg);
    for (uint32_t r = 0; r < p->rows; r++) {
        for (uint32_t c = 0; c < p->cols; c++) {
            int chc; uint8_t a;
            pane_cell(p, r, c, &chc, &a);
            if ((chc == ' ' || chc == 0) && (a & 7) == PC_BLACK) continue;
            uint32_t fg = pane_palette[(a >> 4) & 7];
            font_draw_glyph_t(cx + c * GW, cy + r * GH, (chc == 0 ? ' ' : chc), fg);
        }
    }
    if (p->scroll == 0 && focus)
        fb_fill_rect(cx + p->cursor_col * GW, cy + p->cursor_row * GH + GH - 2, GW, 2, TH->glow);
    else if (p->scroll != 0)
        fb_fill_rect(cx + cw - 6, cy + 2, 4, 4, TH->glow);
}

// A cheap fingerprint of everything render_pane() would draw for this pane.
// Hashing a screenful of characters is two orders of magnitude cheaper than
// rasterising a screenful of anti-aliased glyphs, so it pays for itself the
// moment a pane holds still for a single frame -- which, on an idle desktop,
// is every frame.
#define FNV_P 1099511628211ULL
static uint64_t pane_fingerprint(const struct pane *p) {
    uint64_t hsh = 14695981039346656037ULL;
    for (uint32_t r = 0; r < p->rows && r < PANE_MAX_ROWS; r++) {
        const uint16_t *cr = p->cells[r];
        const uint8_t  *ar = p->attr[r];
        for (uint32_t c = 0; c < p->cols && c < PANE_MAX_COLS; c++) {
            hsh = (hsh ^ cr[c]) * FNV_P;
            hsh = (hsh ^ ar[c]) * FNV_P;
        }
    }
    hsh = (hsh ^ p->cursor_row) * FNV_P;
    hsh = (hsh ^ p->cursor_col) * FNV_P;
    hsh = (hsh ^ p->scroll)     * FNV_P;   // viewport into the scrollback
    hsh = (hsh ^ p->hist_head)  * FNV_P;   // history changed under the viewport
    hsh = (hsh ^ p->hist_count) * FNV_P;
    return hsh;
}

// What each pane looked like the last time it was painted.
struct pane_cache {
    int      valid;
    uint64_t fp;
    uint32_t x, y, w, h;
    int      focus, theme;
};
static struct pane_cache pcache[MAX_PANES];

static void invalidate_pane_cache(void) {
    for (int i = 0; i < MAX_PANES; i++) pcache[i].valid = 0;
}

#define BTN_W  30                   // title-bar button cell
#define GRAB   5                    // how close to an edge counts as a resize

// Title-bar button rectangles, right-aligned like Windows: minimise, maximise,
// close. Returns the x of the leftmost button.
static int win_btn_x(const struct wm_node *nd, int which) {
    int right = (int)(nd->x + nd->w) - FRAME - 4;
    return right - (3 - which) * BTN_W;      // which: 0 min, 1 max, 2 close
}

// Returns 1 if it actually painted. Overlapping windows make that matter: once
// one window repaints, every window stacked above it must repaint too, or the
// one behind would be left drawn on top of it.
uint64_t pf_fp, pf_paint, pf_content, pf_blit;
int      pf_painted, pf_skipped, pf_blits;

static int render_pane(int n, int force) {
    struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    const theme_t *T = TH;
    int focus = (n == focused);
    uint32_t x = nd->x, y = nd->y, w = nd->w, h = nd->h;

    // Skip the pane entirely when nothing about it changed: its pixels are
    // still sitting in the back buffer from last frame. Graphics-mode panes
    // (DOOM) are never skipped -- their pixel buffer is not part of the
    // fingerprint.
    struct pane_cache *pc = &pcache[nd->pane_idx];
    uint64_t f0 = rdtsc();
    if (!p->gfx_on && !force) {
        uint64_t fp = pane_fingerprint(p);
        pf_fp += rdtsc() - f0;
        if (pc->valid && pc->fp == fp && pc->x == x && pc->y == y &&
            pc->w == w && pc->h == h && pc->focus == focus && pc->theme == theme) {
            pf_skipped++;
            return 0;
        }
        pc->valid = 1; pc->fp = fp;
        pc->x = x; pc->y = y; pc->w = w; pc->h = h;
        pc->focus = focus; pc->theme = theme;
    } else {
        pc->valid = 0;
        if (!p->gfx_on) {                 // forced repaint: refresh the cache
            pc->valid = 1; pc->fp = pane_fingerprint(p);
            pc->x = x; pc->y = y; pc->w = w; pc->h = h;
            pc->focus = focus; pc->theme = theme;
        }
    }
    pf_painted++;
    uint64_t p0 = rdtsc();
    fb_mark_rect(x, y, w, h);   // covers every helper this function draws with

    // Window body background -- but only the parts nothing else will cover.
    //
    // Filling the whole window and then painting the content over it means
    // writing most of those pixels twice. On a 1180x580 window that is 684
    // thousand pixels of pure waste per frame; the frame margins around the
    // content are about eighteen thousand.
    {
        uint32_t bx, by2, bw, bh;
        content_rect(n, &bx, &by2, &bw, &bh);
        if (bw && bh) {
            uint32_t left = bx - x, top = by2 - y;
            if (left)  fb_fill_rect(x, by2, left, bh, T->content_bg);
            uint32_t rightx = bx + bw, rightw = (x + w > rightx) ? x + w - rightx : 0;
            if (rightw) fb_fill_rect(rightx, by2, rightw, bh, T->content_bg);
            uint32_t boty = by2 + bh, both = (y + h > boty) ? y + h - boty : 0;
            if (both) fb_fill_rect(x, boty, w, both, T->content_bg);
            (void)top;              // the title bar gradient covers it
        } else {
            fb_fill_rect(x, y, w, h, T->content_bg);
        }
    }
    fill_vgrad(x, y, w, TITLE_H, T->title_a, T->title_b);
    fb_fill_rect(x, y, w, 1, T->title_hl);                 // top gloss line
    fb_fill_rect(x, y + TITLE_H, w, 1, mix(T->title_b, 0x000000, 60));

    // Soft glow frame: a 2px edge, brighter when focused.
    uint32_t edge = focus ? T->glow : T->glow_dim;
    fb_fill_rect(x, y, w, EDGE, edge);
    fb_fill_rect(x, y + h - EDGE, w, EDGE, edge);
    fb_fill_rect(x, y, EDGE, h, edge);
    fb_fill_rect(x + w - EDGE, y, EDGE, h, edge);

    // Title text (foreground program name).
    int ty = (int)y + (TITLE_H - (int)GH) / 2;
    {
        const char *ttl = pane_title(p);
        int tx0 = (int)x + 12;
        if (icon_draw((int)x + 8, (int)y + (TITLE_H - ICON_SMALL) / 2, ttl, ICON_SMALL))
            tx0 = (int)x + 8 + ICON_SMALL + 8;
        draw_text_t(tx0, ty, ttl, focus ? T->title_text : T->title_text_dim);
    }

    // Minimise / maximise / close, right-aligned in the title bar.
    {
        int by = (int)y + (TITLE_H - 18) / 2;
        uint32_t ink = focus ? T->title_text : T->title_text_dim;
        for (int b = 0; b < 3; b++) {
            int bx = win_btn_x(nd, b);
            int cxm = bx + (BTN_W - 4) / 2, cym = by + 9;
            if (b == 2) fb_fill_rect(bx, by, BTN_W - 4, 18, mix(T->title_a, 0x00C0392B, 90));
            switch (b) {
                case 0:                                   // minimise: a bar
                    fb_fill_rect(cxm - 5, cym + 4, 10, 2, ink);
                    break;
                case 1:                                   // maximise: a box
                    fb_fill_rect(cxm - 5, cym - 5, 10, 1, ink);
                    fb_fill_rect(cxm - 5, cym + 4, 10, 1, ink);
                    fb_fill_rect(cxm - 5, cym - 5, 1, 10, ink);
                    fb_fill_rect(cxm + 4, cym - 5, 1, 10, ink);
                    break;
                default:                                  // close: an X
                    for (int k = 0; k < 9; k++) {
                        fb_fill_rect(cxm - 4 + k, cym - 4 + k, 1, 1, 0x00FFFFFF);
                        fb_fill_rect(cxm + 4 - k, cym - 4 + k, 1, 1, 0x00FFFFFF);
                    }
                    break;
            }
        }
    }

    // Content.
    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    uint64_t c0 = rdtsc();
    draw_content(n, cx, cy, cw, ch, focus);
    pf_content += rdtsc() - c0;
    pf_paint += rdtsc() - p0;

    round_corners(x, y, w, h, 6);
    return 1;
}
static void render_windows(void) {
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    int force = 0;
    for (int i = 0; i < n; i++) {
        if (nodes[win[i]].state == WIN_MIN) continue;
        if (render_pane(win[i], force)) force = 1;
    }
}

// Cached wall clock, refreshed once a second by wm_poll so the render path
// never has to poke the CMOS RTC (which can stall briefly mid-update).
static struct rtc_time wm_clock;

// --- wallpaper (theme-aware, cached) --------------------------------------
static int wp_theme = -1;
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Read one decimal number from a PPM header, skipping whitespace and #comments.
static int ppm_num(int fd, uint32_t *out) {
    char c;
    uint32_t v = 0;
    int got = 0;
    for (;;) {
        if (vfs_read(fd, &c, 1) != 1) return 0;
        if (c == '#') { while (vfs_read(fd, &c, 1) == 1 && c != '\n') { } continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (got) break;
            continue;
        }
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (uint32_t)(c - '0');
        got = 1;
        if (v > 100000) return 0;
    }
    *out = v;
    return got;
}

// Load a binary PPM (P6) wallpaper from `path`, nearest-neighbour scaled to the
// screen. One source row is held at a time, so a multi-megabyte photo costs
// almost no memory -- and since reads stream, the file can live on the USB
// stick. Returns 1 if the wallpaper was filled in.
static int load_wallpaper_file(const char *path, uint32_t w, uint32_t h) {
    int fd = vfs_open(path);
    if (fd < 0) return 0;

    char m0 = 0, m1 = 0;
    uint32_t iw = 0, ih = 0, mx = 0;
    int ok = vfs_read(fd, &m0, 1) == 1 && vfs_read(fd, &m1, 1) == 1 &&
             m0 == 'P' && m1 == '6' &&
             ppm_num(fd, &iw) && ppm_num(fd, &ih) && ppm_num(fd, &mx) &&
             iw > 0 && ih > 0 && mx == 255;
    if (!ok) { vfs_close(fd); return 0; }

    // ppm_num consumed the single whitespace byte that separates the header
    // from the pixel data, so the data starts here.
    int64_t data_off = vfs_tell(fd);
    uint32_t rowbytes = iw * 3;
    uint8_t *row = (uint8_t *)kmalloc(rowbytes);
    if (!row) { vfs_close(fd); return 0; }

    int filled = 1;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * ih / h);
        if (vfs_seek(fd, data_off + (int64_t)sy * rowbytes, VFS_SEEK_SET) < 0 ||
            vfs_read(fd, row, rowbytes) != (int32_t)rowbytes) {
            filled = (y > 0);          // a truncated file still gives us a picture
            break;
        }
        uint32_t *drow = wallpaper + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px = row + (uint32_t)((uint64_t)x * iw / w) * 3;
            drow[x] = ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }
    }
    kfree(row);
    vfs_close(fd);
    return filled;
}

static void gen_wallpaper(uint32_t w, uint32_t h) {
    if (wallpaper) kfree(wallpaper);
    wallpaper = kmalloc((size_t)w * h * 4);
    wp_w = wallpaper ? w : 0;
    wp_h = wallpaper ? h : 0;
    wp_theme = theme;
    if (!wallpaper) return;

    // A user picture wins over the generated gradient: drop a binary PPM at
    // /wall.ppm. The stick is only consulted when it is ALREADY mounted --
    // reaching for /usb here would auto-mount it during the very first repaint
    // and put SCSI traffic in every boot, which the lazy mount exists to avoid.
    if (load_wallpaper_file("/wall.ppm", w, h)) return;
    if (fat32_mounted() && load_wallpaper_file("/usb/wall.ppm", w, h)) return;

    const theme_t *T = TH;
    int ar = (T->wp_glow >> 16) & 255, ag = (T->wp_glow >> 8) & 255, ab = T->wp_glow & 255;
    int cx = (int)(w / 2), cy = (int)(h * 32 / 100);
    long R = (long)(h * 82 / 100); if (R < 1) R = 1;
    long R2 = R * R;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t basecol = mix(T->wp_a, T->wp_b, (int)(y * 256 / h));
        int br = (basecol >> 16) & 255, bgc = (basecol >> 8) & 255, bb = basecol & 255;
        uint32_t *row = wallpaper + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            long dx = (long)x - cx, dy = (long)y - cy, d2 = dx * dx + dy * dy;
            int rr = br, gg = bgc, bl = bb;
            if (d2 < R2) {
                int gl = (int)((R2 - d2) * 46 / R2);
                rr += ar * gl / 255; gg += ag * gl / 255; bl += ab * gl / 255;
            }
            // A soft diagonal light streak for a bit of Aero "flow".
            int s = (int)(x + y);
            int band = (s % (int)(w / 2 + 1));
            if (band < 90) { int add = (90 - band) / 6; rr += add; gg += add; bl += add; }
            row[x] = ((uint32_t)clamp8(rr) << 16) | ((uint32_t)clamp8(gg) << 8) | clamp8(bl);
        }
    }
}

static void draw_wallpaper(void) {
    uint32_t w = fb_get_width(), h = fb_get_height();
    if (!wallpaper || wp_w != w || wp_h != h || wp_theme != theme) {
        gen_wallpaper(w, h);
        wp_dirty = 1;
    }
    if (!wallpaper) { fb_fill_rect(0, 0, w, h, COLOR_DESKTOP); return; }
    if (!wp_dirty) return;                  // already sitting in the back buffer
    wp_dirty = 0;
    fb_mark_rows(0, h);

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    uint32_t pairs = w / 2;                 // copy two pixels at a time
    for (uint32_t y = 0; y < h; y++) {
        volatile uint64_t *drow = (volatile uint64_t *)(base + (size_t)y * pitch);
        const uint64_t *srow = (const uint64_t *)(wallpaper + (size_t)y * w);
        for (uint32_t i = 0; i < pairs; i++) drow[i] = srow[i];
        if (w & 1) {                        // odd width: the last pixel
            volatile uint32_t *d32 = (volatile uint32_t *)(base + (size_t)y * pitch);
            d32[w - 1] = wallpaper[(size_t)y * w + w - 1];
        }
    }
}

// --- small text/number helpers --------------------------------------------
static int u2s(char *b, unsigned v) {
    char t[12]; int n = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return 1; }
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0; return n;
}
static void sapp(char *b, int *pos, const char *s) { while (*s) b[(*pos)++] = *s++; b[*pos] = 0; }
static void napp(char *b, int *pos, unsigned v) { *pos += u2s(b + *pos, v); }
static void clock_str(char *b) {
    b[0] = '0' + (wm_clock.hour / 10) % 10; b[1] = '0' + wm_clock.hour % 10; b[2] = ':';
    b[3] = '0' + (wm_clock.min / 10) % 10;  b[4] = '0' + wm_clock.min % 10;  b[5] = ':';
    b[6] = '0' + (wm_clock.sec / 10) % 10;  b[7] = '0' + wm_clock.sec % 10;  b[8] = 0;
}

// A glossy filled circle (the Start orb).
static void draw_orb(int cx, int cy, int r, uint32_t c) {
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch(), fbw = fb_get_width(), fbh = fb_get_height();
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
        if (dx*dx + dy*dy > r*r) continue;
        int px = cx + dx, py = cy + dy;
        if (px < 0 || py < 0 || (uint32_t)px >= fbw || (uint32_t)py >= fbh) continue;
        int t = (dy + r) * 256 / (2 * r);                  // gloss top->bottom
        uint32_t col = mix(mix(c, 0x00FFFFFF, 110), mix(c, 0, 90), t);
        *(volatile uint32_t *)(base + py * pitch + px * 4) = col;
    }
}

// A little time-series graph: filled bars, oldest -> newest left to right.
static void draw_graph(int x, int y, int w, int h, const uint8_t *hist,
                       uint32_t c_hi, uint32_t c_lo) {
    fb_fill_rect(x, y, w, h, 0x0010141C);                  // track
    fb_fill_rect(x, y + h / 2, w, 1, 0x001C2230);          // 50% gridline
    int bw = w / HIST; if (bw < 1) bw = 1;
    for (int k = 0; k < hist_count; k++) {
        int s = hist[(hist_pos - hist_count + k + HIST * 4) % HIST];
        int bx = x + w - (hist_count - k) * bw;
        if (bx < x) continue;
        int bh = s * h / 100; if (bh < 1 && s > 0) bh = 1;
        fill_vgrad(bx, y + h - bh, bw, bh, c_hi, c_lo);
    }
    fb_fill_rect(x, y, w, 1, 0x00242C3A);                  // frame
    fb_fill_rect(x, y + h - 1, w, 1, 0x00242C3A);
}

// Live "System Monitor" gadget: a frosted-glass panel above the clock with
// CPU and memory history graphs. Toggled with Super+M.
static void draw_gadget(void) {
    const theme_t *T = TH;
    if (!wallpaper) return;
    uint32_t fw = fb_get_width(), fh = fb_get_height();

    // The core graphs are laid out in a grid, so the gadget's height depends on
    // how many CPUs there are: 1 in QEMU, up to 16 on real silicon. A fixed
    // height clipped the lower half (MEM graph + footer) on a many-core box, so
    // size the box to its content and only then place it above the taskbar.
    const int ghd = 22;                       // per-core graph height
    int nc = smp_cpu_count();
    if (nc < 1) nc = 1;
    if (nc > UI_CORES) nc = UI_CORES;
    int gcols = (nc >= 4) ? 4 : nc;
    int grows = (nc + gcols - 1) / gcols;

    int gw = 244;
    int gh = 214 + (int)GH + 6 + (grows - 1) * (ghd + 4);  // +1 row for FPS
    int gx = (int)fw - gw - AGAP;
    int gy = (int)fh - TASKBAR_H - gh - AGAP;             // sit just above the clock
    if (gy < (int)BAR_H + AGAP) gy = (int)BAR_H + AGAP;   // never run off the top
    if (gx < 0 || gy < 0) return;

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    fb_mark_rows((uint32_t)gy, (uint32_t)gh);   // this loop writes the buffer directly
    for (int j = 0; j < gh; j++) {
        int py = gy + j; if (py < 0 || (uint32_t)py >= wp_h) continue;
        volatile uint32_t *drow = (volatile uint32_t *)(base + py * pitch);
        for (int i = 0; i < gw; i++) {
            int px = gx + i; if (px < 0 || (uint32_t)px >= wp_w) continue;
            drow[px] = mix(wallpaper[py * wp_w + px], 0x000000, 160);
        }
    }
    // Glass edge.
    fb_fill_rect(gx, gy, gw, 2, T->glow);
    fb_fill_rect(gx, gy + gh - 1, gw, 1, mix(T->glow, 0, 130));
    fb_fill_rect(gx, gy, 1, gh, mix(T->glow, 0, 60));
    fb_fill_rect(gx + gw - 1, gy, 1, gh, mix(T->glow, 0, 130));

    int tx = gx + 12, yy = gy + 8;
    draw_text_t(tx, yy, "System Monitor", T->glow);
    yy += (int)GH + 6;

    char v[8]; int q;
    // CPU -- one bar per core, so all cores are visible at once.
    draw_text_t(tx, yy, "CPU", 0x0066C2E0);
    q = 0; napp(v, &q, cur_cpu); sapp(v, &q, "%");
    draw_text_t(gx + gw - 12 - q * (int)GW, yy, v, 0x00E6ECF5);   // overall load
    yy += (int)GH + 4;
    {
        // A separate history graph per core, laid out in a grid (dimensions
        // computed at the top so the box was sized to hold them).
        int cellw = (gw - 24) / gcols;
        for (int c = 0; c < nc; c++) {
            int cx = tx + (c % gcols) * cellw;
            int cyy = yy + (c / gcols) * (ghd + 4);
            draw_graph(cx, cyy, cellw - 4, ghd, core_hist[c], 0x0066C2E0, 0x00143A54);
        }
        yy += grows * (ghd + 4) + 6;
    }
    // MEM
    draw_text_t(tx, yy, "MEM", 0x009ECE6A);
    q = 0; napp(v, &q, cur_mem); sapp(v, &q, "%");
    draw_text_t(gx + gw - 12 - q * (int)GW, yy, v, 0x00E6ECF5);
    yy += (int)GH + 3;
    draw_graph(tx, yy, gw - 24, 34, mem_hist, 0x009ECE6A, 0x00203A18);
    yy += 34 + 6;

    // FPS -- frames actually put on the screen over the last second, not
    // frames asked for. An idle desktop draws almost nothing and shows a
    // single digit here; that is the honest answer, and Super+B measures the
    // ceiling when you want to know what it COULD do.
    draw_text_t(tx, yy, "FPS", 0x00E0C060);
    q = 0; napp(v, &q, (unsigned)cur_fps);
    draw_text_t(gx + gw - 12 - q * (int)GW, yy, v, 0x00E6ECF5);
    yy += (int)GH + 6;

    // Footer: process count + uptime.
    static struct si_proc procs[24];
    int nproc = process_list(procs, 24);
    uint64_t up = pit_get_ticks() / 100;
    char line[32]; int pos = 0;
    sapp(line, &pos, "PROC "); napp(line, &pos, (unsigned)nproc);
    draw_text_t(tx, yy, line, 0x00B8C0CC);
    char ut[16]; q = 0;
    napp(ut, &q, (unsigned)(up / 3600)); sapp(ut, &q, ":");
    if ((up / 60) % 60 < 10) sapp(ut, &q, "0"); napp(ut, &q, (unsigned)((up / 60) % 60)); sapp(ut, &q, ":");
    if (up % 60 < 10) sapp(ut, &q, "0"); napp(ut, &q, (unsigned)(up % 60));
    draw_text_t(gx + gw - 12 - q * (int)GW, yy, ut, 0x00B8C0CC);
}

// --- bottom taskbar -------------------------------------------------------
// Everything the taskbar shows, folded into one value. It only really changes
// once a second (the clock), so this keeps an otherwise idle desktop from
// repainting -- and therefore from pushing anything to the screen at all.
static uint64_t taskbar_fingerprint(void) {
    uint64_t hsh = 14695981039346656037ULL;
    hsh = (hsh ^ (uint64_t)theme)  * FNV_P;
    hsh = (hsh ^ (uint64_t)cur_ws) * FNV_P;
    hsh = (hsh ^ (uint64_t)focused) * FNV_P;
    for (int i = 0; i < MAX_WS; i++)
        hsh = (hsh ^ (uint64_t)ws_has_windows(i)) * FNV_P;

    int leaves[MAX_PANES];
    int n = collect_windows(cur_ws, leaves, MAX_PANES);
    for (int i = 0; i < n; i++) {
        hsh = (hsh ^ (uint64_t)leaves[i]) * FNV_P;
        const char *t = pane_title(&panes[nodes[leaves[i]].pane_idx]);
        for (int k = 0; t[k] && k < 12; k++) hsh = (hsh ^ (uint8_t)t[k]) * FNV_P;
    }

    uint64_t total = pmm_total_frame_count(), freef = pmm_free_frame_count();
    unsigned used_pct = total ? (unsigned)((total - freef) * 100 / total) : 0;
    static struct si_proc fp_procs[24];
    hsh = (hsh ^ used_pct) * FNV_P;
    hsh = (hsh ^ (uint64_t)process_list(fp_procs, 24)) * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.hour) * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.min)  * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.sec)  * FNV_P;
    return hsh;
}

static uint64_t tb_fp;
static int      tb_valid = 0;

static void draw_taskbar(void) {
    const theme_t *T = TH;
    uint32_t w = fb_get_width(), h = fb_get_height();
    int by = (int)h - TASKBAR_H;

    uint64_t fp = taskbar_fingerprint();
    if (tb_valid && tb_fp == fp) return;      // identical to what is on screen
    tb_fp = fp; tb_valid = 1;
    fill_vgrad(0, by, w, TASKBAR_H, T->bar_a, T->bar_b);
    fb_fill_rect(0, by, w, 1, T->title_hl);                 // top gloss
    int ty = by + (TASKBAR_H - (int)GH) / 2;

    // Start orb + label.
    int orb_r = (TASKBAR_H - 14) / 2;
    int ocx = 8 + orb_r, ocy = by + TASKBAR_H / 2;
    // The drawn logo if there is one, the painted orb if there is not.
    if (!icon_draw(ocx - ICON_BIG / 2, ocy - ICON_BIG / 2, "start", ICON_BIG))
        draw_orb(ocx, ocy, orb_r, T->orb);
    draw_text_t(ocx + orb_r + 8, ty, "xyuOS", T->bar_text);
    int bx = ocx + orb_r + 8 + 6 * (int)GW + 14;

    // Workspace pills: the current one lit, plus any other that has windows.
    for (int i = 0; i < MAX_WS; i++) {
        if (i != cur_ws && !ws_has_windows(i)) continue;   // nothing there
        int pw = (int)GW + 12, pyy = by + 8, phh = TASKBAR_H - 16;
        int here = (i == cur_ws);
        if (here) fill_vgrad(bx, pyy, pw, phh, mix(T->glow, 0x00FFFFFF, 60), T->glow);
        else      fill_vgrad(bx, pyy, pw, phh, mix(T->bar_a, 0x00FFFFFF, 20), T->bar_b);
        fb_fill_rect(bx, pyy, pw, 1, here ? T->title_hl : mix(T->bar_a, 0x00FFFFFF, 40));
        char wl[2] = { (char)('1' + i), 0 };
        draw_text_t(bx + 6, ty, wl, here ? 0x00FFFFFF : T->bar_dim);
        bx += pw + 4;
    }
    bx += 10;

    // One button per open window -- of THIS workspace only.
    int wsleaves[MAX_PANES];
    int nleaves = collect_windows(cur_ws, wsleaves, MAX_PANES);
    for (int li = 0; li < nleaves; li++) {
        int i = wsleaves[li];
        struct pane *p = &panes[nodes[i].pane_idx];
        const char *t = pane_title(p);
        int len = 0; while (t[len] && len < 12) len++;
        int bw = len * (int)GW + 20;
        if (icon_get(t, ICON_SMALL)) bw += ICON_SMALL + 6;
        int active = (i == focused);
        int byy = by + 6, bhh = TASKBAR_H - 12;
        if (active) fill_vgrad(bx, byy, bw, bhh, mix(T->glow, 0x00FFFFFF, 60), T->glow);
        else        fill_vgrad(bx, byy, bw, bhh, mix(T->bar_a, 0x00FFFFFF, 24), T->bar_b);
        fb_fill_rect(bx, byy, bw, 1, active ? T->title_hl : mix(T->bar_a, 0x00FFFFFF, 40));
        fb_fill_rect(bx, byy, 1, bhh, mix(T->bar_a, 0, 40));
        char lbl[16]; int q = 0; for (int k = 0; k < len; k++) lbl[q++] = t[k]; lbl[q] = 0;
        int tx0 = bx + 10;
        if (icon_draw(bx + 8, byy + (bhh - ICON_SMALL) / 2, t, ICON_SMALL))
            tx0 = bx + 10 + ICON_SMALL + 6;
        draw_text_t(tx0, ty, lbl, active ? 0x00FFFFFF : T->bar_text);
        bx += bw + 6;
    }

    // Tray: live memory + process count, then the clock.
    char tray[40]; int pos = 0;
    uint64_t total = pmm_total_frame_count(), freef = pmm_free_frame_count();
    unsigned used_pct = total ? (unsigned)((total - freef) * 100 / total) : 0;
    static struct si_proc procs[24];
    int nproc = process_list(procs, 24);
    sapp(tray, &pos, "MEM "); napp(tray, &pos, used_pct); sapp(tray, &pos, "%  PROC ");
    napp(tray, &pos, (unsigned)nproc);
    char clk[9]; clock_str(clk);
    int clk_x = (int)w - 8 * (int)GW - 12;
    int tray_x = clk_x - pos * (int)GW - 18;
    draw_text_t(tray_x, ty, tray, T->bar_dim);
    draw_text_t(clk_x, ty, clk, T->bar_text);

    // The whole bar, because the whole bar was just repainted.
    //
    // Most of what is drawn above marks its own rectangle, but fill_vgrad
    // writes the buffer directly and marks nothing -- so what reached the
    // screen was only the parts under the glyphs and the hairlines. That is
    // invisible until a button gets SHORTER: the new label was pushed over
    // the first few letters of the old one and the rest of it stayed on the
    // screen for ever, because from then on the buffer and the screen agreed
    // everywhere the new frame touched. "shgchild" is what that looks like.
    fb_mark_rows((uint32_t)by, TASKBAR_H);
}

// Refresh the clock + CPU/memory history, at most once a second. Called from
// both the idle path (wm_poll) and the render path (so it keeps advancing even
// while a CPU-bound program like DOOM is pinning the compositor). Returns 1 if
// it actually sampled this call.
static int sample_stats(void) {
    static uint64_t s_last = 0;
    uint64_t now = pit_get_ticks();
    if (s_last && now - s_last < 100) return 0;

    static uint64_t last_ticks = 0, last_idle = 0;
    uint64_t idl = pit_idle_ticks();
    uint64_t dt = now - last_ticks, di = idl - last_idle;
    int cpu = (last_ticks && dt > 0) ? (int)(100 - (di * 100 / dt)) : 0;
    if (cpu < 0) cpu = 0; else if (cpu > 100) cpu = 100;

    // The PIT ticks at 100 Hz, so frames * 100 / dt is frames per second.
    static uint32_t last_frames = 0;
    uint32_t fnow = fb_frames_pushed();
    if (last_ticks && dt > 0) {
        uint64_t f = (uint64_t)(fnow - last_frames) * 100 / dt;
        cur_fps = (uint16_t)(f > 65535 ? 65535 : f);
    }
    last_frames = fnow;

    last_ticks = now; last_idle = idl;
    s_last = now;

    rtc_read(&wm_clock);
    uint64_t total = pmm_total_frame_count(), freef = pmm_free_frame_count();
    int mem = total ? (int)((total - freef) * 100 / total) : 0;
    cur_cpu = (uint8_t)cpu; cur_mem = (uint8_t)mem;
    cpu_hist[hist_pos] = cur_cpu; mem_hist[hist_pos] = cur_mem;
    int nch = smp_cpu_count(); if (nch > UI_CORES) nch = UI_CORES;
    for (int c = 0; c < nch; c++) core_hist[c][hist_pos] = (uint8_t)smp_core_load(c);
    hist_pos = (hist_pos + 1) % HIST;
    if (hist_count < HIST) hist_count++;
    return 1;
}

// A frame is mostly decisions about what NOT to draw. Each stage compares what
// it is about to paint against what is already in the back buffer and bails out
// when they match; fb_present() then pushes only the scanlines that really
// changed. An idle desktop therefore costs a few tens of microseconds instead
// of repainting eight megabytes.
// The pointer, drawn last so it floats over every window. A classic arrow:
// each row is a run of solid pixels with a one-pixel outline, scaled up a
// little so it is visible on a 1080p panel.
#define CUR_W 12
#define CUR_H 19
static const char *cursor_bits[CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X......XXXXX",
    "X...X..X    ",
    "X..X X..X   ",
    "X.X  X..X   ",
    "XX    X..X  ",
    "X     X..X  ",
    "       X..X ",
    "       XXXX ",
};

// Where a dragged window would land: a tinted pane with an accent outline,
// the same affordance Windows shows when you shove a window at an edge.
static void draw_snap_preview(void) {
    if (snap_hint == SNAP_NONE) return;
    const theme_t *T = TH;
    uint32_t x, y, w, h;
    snap_rect(snap_hint, &x, &y, &w, &h);

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (uint32_t yy = y; yy < y + h && yy < fb_get_height(); yy++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (uint32_t xx = x; xx < x + w && xx < fb_get_width(); xx++)
            row[xx] = mix(row[xx], T->glow, 90);
    }
    fb_fill_rect(x, y, w, 3, T->glow);
    fb_fill_rect(x, y + h - 3, w, 3, T->glow);
    fb_fill_rect(x, y, 3, h, T->glow);
    fb_fill_rect(x + w - 3, y, 3, h, T->glow);
    fb_mark_rows(y, h);
}

// The Alt+Tab list, shown for as long as Alt is held down.
static int alttab_active = 0;

// --- drag and drop --------------------------------------------------------
//
// The drag belongs to the compositor, not to the program that started it.
// That is forced by the geometry: the moment the pointer crosses out of the
// source window, that window stops receiving pointer events, so it cannot
// track where the thing is going, cannot draw it, and cannot know what it was
// let go on. Only the thing that owns the whole screen can.
//
// So a program says "I am dragging this", and the compositor takes over:
// draws the label under the cursor, tells whichever window the pointer is
// over that something is hovering, and on release leaves the payload in that
// window's pane for it to collect.

#define DRAG_LABEL_MAX 48

static int  udrag_active;
static int  udrag_over = -1;         // the window the drag is currently over
static char udrag_payload[PANE_DROP_MAX];
static char udrag_label[DRAG_LABEL_MAX];

static void str_copy_n(char *d, const char *s2, int max) {
    int i = 0;
    if (s2) while (s2[i] && i < max - 1) { d[i] = s2[i]; i++; }
    d[i] = 0;
}

int wm_drag_active(void) { return udrag_active; }

int wm_drag_begin(const char *payload, const char *label) {
    // Copy out of the caller's address space now: the pointer is a user
    // pointer, and this outlives the syscall that started it.
    if (!payload) return -1;
    str_copy_n(udrag_payload, payload, PANE_DROP_MAX);
    str_copy_n(udrag_label, label && label[0] ? label : payload, DRAG_LABEL_MAX);
    udrag_active = 1;
    wp_dirty = 1;
    dirty = 1;
    return 0;
}

void wm_drag_cancel(void) {
    if (!udrag_active) return;
    udrag_active = 0;
    udrag_over = -1;
    wp_dirty = 1;
    dirty = 1;
}

int wm_drop_take(struct pane *p, char *out, int max) {
    if (!p->drop_ready || max <= 0) return -1;
    int i = 0;
    while (p->drop[i] && i < max - 1) { out[i] = p->drop[i]; i++; }
    out[i] = 0;
    p->drop_ready = 0;
    return i;
}

// What follows the cursor: the name of the thing being dragged, in a small
// slab. Without it a drag is invisible and the user is dragging on faith.
static void draw_drag_ghost(void) {
    if (!udrag_active) return;
    int32_t mx, my;
    mouse_position(&mx, &my);

    int len = 0;
    while (udrag_label[len]) len++;
    int w = len * (int)GW + 18, h = (int)GH + 12;
    int x = mx + 16, y = my + 16;
    if (x + w > (int)fb_get_width())  x = (int)fb_get_width() - w;
    if (y + h > (int)fb_get_height()) y = (int)fb_get_height() - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || (uint32_t)yy >= fb_get_height()) continue;
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || (uint32_t)xx >= fb_get_width()) continue;
            row[xx] = mix(row[xx], TH->menu_bg, 190);
        }
    }
    fb_mark_rows(y, h);
    fb_fill_rect(x, y, w, 1, TH->glow);
    fb_fill_rect(x, y + h - 1, w, 1, TH->glow);
    fb_fill_rect(x, y, 1, h, TH->glow);
    fb_fill_rect(x + w - 1, y, 1, h, TH->glow);
    draw_text_t(x + 9, y + 6, udrag_label, 0x00FFFFFF);
}

// --- message boxes --------------------------------------------------------
//
// A small queue rather than a single slot: a crashing program can fault more
// than once on its way down, and the second report must not overwrite the
// first one before anybody has read it.

#define MBOX_QUEUE  4
#define MBOX_TEXT   96
#define MBOX_CODE   24

struct mbox {
    int  used, kind;
    char code[MBOX_CODE];
    char title[40];
    char text[MBOX_TEXT];
    char detail[MBOX_TEXT];
};

static struct mbox mboxes[MBOX_QUEUE];
static int mbox_head;              // the one being shown

static void mb_copy(char *d, const char *s2, int max) {
    int i = 0;
    if (s2) while (s2[i] && i < max - 1) { d[i] = s2[i]; i++; }
    d[i] = 0;
}

int wm_message_pending(void) { return mboxes[mbox_head].used; }

void wm_message_box(int kind, const char *code, const char *title,
                    const char *text, const char *detail) {
    int slot = -1;
    for (int i = 0; i < MBOX_QUEUE; i++) {
        int k = (mbox_head + i) % MBOX_QUEUE;
        if (!mboxes[k].used) { slot = k; break; }
    }
    if (slot < 0) return;                        // queue full: drop the newest
    struct mbox *m = &mboxes[slot];
    m->used = 1;
    m->kind = kind;
    mb_copy(m->code, code, MBOX_CODE);
    mb_copy(m->title, title, sizeof m->title);
    mb_copy(m->text, text, MBOX_TEXT);
    mb_copy(m->detail, detail, MBOX_TEXT);
    // Whatever the dialog covers has to be repainted when it goes away.
    wp_dirty = 1;
    dirty = 1;
    sound_alert(kind);
}

static void mbox_dismiss(void) {
    if (!mboxes[mbox_head].used) return;
    mboxes[mbox_head].used = 0;
    mbox_head = (mbox_head + 1) % MBOX_QUEUE;
    wp_dirty = 1;
    dirty = 1;
}

static int mb_len(const char *s2) {
    int n = 0;
    while (s2 && s2[n]) n++;
    return n;
}

static void mbox_geom(int *x, int *y, int *w, int *h) {
    // Size to the longest line rather than to a guess. A fixed width is fine
    // until the first message that does not fit, and then it is wrong in the
    // one situation where the text matters most.
    struct mbox *m = &mboxes[mbox_head];
    int cols = mb_len(m->text);
    int d = mb_len(m->detail);
    if (d > cols) cols = d;
    if (cols < 36) cols = 36;
    *w = (cols + 8) * (int)GW + 60;
    if (*w > (int)fb_get_width() - 60) *w = (int)fb_get_width() - 60;
    *h = (int)GH * 4 + 96;
    *x = ((int)fb_get_width() - *w) / 2;
    *y = ((int)fb_get_height() - *h) / 2 - 40;
    if (*y < 20) *y = 20;
}

static void mbox_ok_rect(int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh;
    mbox_geom(&dx, &dy, &dw, &dh);
    *w = (int)GW * 10;
    *h = (int)GH + 14;
    *x = dx + dw - *w - 16;
    *y = dy + dh - *h - 14;
}

// The icon carries the severity before a single word is read, which is the
// entire reason message boxes have one.
static void draw_mbox_icon(int cx, int cy, int r, int kind) {
    uint32_t body = kind == MB_ERROR ? 0x00C0392B :
                    kind == MB_WARN  ? 0x00E0A030 : 0x002A6FD6;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            int px = cx + x, py = cy + y;
            if (px < 0 || py < 0 || (uint32_t)px >= fb_get_width() ||
                (uint32_t)py >= fb_get_height()) continue;
            fb_fill_rect(px, py, 1, 1, y < 0 ? mix(body, 0x00FFFFFF, 40) : body);
        }
    }
    if (kind == MB_ERROR) {                      // an X
        for (int i = -r / 2; i <= r / 2; i++) {
            fb_fill_rect(cx + i - 1, cy + i - 1, 3, 3, 0x00FFFFFF);
            fb_fill_rect(cx + i - 1, cy - i - 1, 3, 3, 0x00FFFFFF);
        }
    } else {                                     // an exclamation / i bar
        int top = kind == MB_WARN ? -r / 2 : -r / 4;
        fb_fill_rect(cx - 2, cy + top, 4, r - 2, 0x00FFFFFF);
        fb_fill_rect(cx - 2, cy + r / 2 + 1, 4, 4, 0x00FFFFFF);
    }
}

static void draw_message_box(void) {
    struct mbox *m = &mboxes[mbox_head];
    if (!m->used) return;


    int x, y, w, h;
    mbox_geom(&x, &y, &w, &h);

    // Dim the whole desktop: this is modal, and it should look modal.
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (uint32_t yy = 0; yy < fb_get_height(); yy++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (uint32_t xx = 0; xx < fb_get_width(); xx++)
            row[xx] = mix(row[xx], 0x00000000, 90);
    }
    fb_mark_rows(0, fb_get_height());

    fb_fill_rect(x, y, w, h, 0x00F2F3F5);
    uint32_t bar = m->kind == MB_ERROR ? 0x00B03428 :
                   m->kind == MB_WARN  ? 0x00B8860B : 0x001B4F9C;
    fill_vgrad(x, y, w, (int)GH + 14, mix(bar, 0x00FFFFFF, 40), bar);
    fb_fill_rect(x, y, w, 1, 0x00FFFFFF);
    draw_text_t(x + 12, y + 7, m->title[0] ? m->title : "Error", 0x00FFFFFF);

    int ix = x + 34, iy = y + (int)GH + 58;
    draw_mbox_icon(ix, iy, 20, m->kind);

    int tx = x + 70, ty = y + (int)GH + 34;
    draw_text_t(tx, ty, m->text, 0x001B1B1F);
    if (m->detail[0])
        draw_text_t(tx, ty + (int)GH + 6, m->detail, 0x006B7280);
    if (m->code[0]) {
        char line[MBOX_CODE + 8];
        int n = 0;
        const char *pre = "code ";
        while (pre[n]) { line[n] = pre[n]; n++; }
        int k = 0;
        while (m->code[k] && n < (int)sizeof line - 1) line[n++] = m->code[k++];
        line[n] = 0;
        draw_text_t(tx, ty + 2 * ((int)GH + 6), line, 0x006B7280);
    }

    int bx, by, bw, bh;
    mbox_ok_rect(&bx, &by, &bw, &bh);
    fill_vgrad(bx, by, bw, bh, 0x00FFFFFF, 0x00E4E8EC);
    fb_fill_rect(bx, by, bw, 1, 0x002A6FD6);
    fb_fill_rect(bx, by + bh - 1, bw, 1, 0x002A6FD6);
    fb_fill_rect(bx, by, 1, bh, 0x002A6FD6);
    fb_fill_rect(bx + bw - 1, by, 1, bh, 0x002A6FD6);
    draw_text_t(bx + (bw - 2 * (int)GW) / 2, by + 7, "OK", 0x001B1B1F);

    fb_fill_rect(x, y, w, 1, 0x00FFFFFF);
    fb_fill_rect(x, y + h - 1, w, 1, 0x00A8B0B8);
    fb_fill_rect(x, y, 1, h, 0x00D0D6DC);
    fb_fill_rect(x + w - 1, y, 1, h, 0x00A8B0B8);
}

// --- the start menu -------------------------------------------------------
//
// Two lists in one: a short curated shelf of the programs a person actually
// launches, and everything else in /bin behind the search box. The catalogue
// is rebuilt each time the menu opens rather than cached, because /bin is a
// directory like any other -- a program compiled a minute ago with `cc` shows
// up without anything having to be told about it.

#define START_MAX  48
#define START_NAME 24
#define START_DESC 40

struct start_item {
    char name[START_NAME];
    char path[48];
    char desc[START_DESC];
    // The file in /bin this came from. The display name may be prettier
    // ("Task manager"), but the icon was drawn against the file name.
    char name_file[START_NAME];
    int  featured;
};

static struct start_item start_items[START_MAX];
static int  start_count;
static int  start_open;
static char start_q[28];
static int  start_qlen;
static int  start_sel;
static int  start_top;

// The programs worth putting a name and a sentence to. Anything in /bin that
// is not here still appears, under its own file name.
static const struct { const char *file, *name, *desc; } start_known[] = {
    { "web",     "Browser",      "Open a page on the web" },
    { "netsurf", "NetSurf",      "The NetSurf browser, ported here" },
    { "files",   "Files",        "Browse folders and open things" },
    { "note",    "Notepad",      "Write and edit text" },
    { "view",    "Viewer",       "Pictures and text files" },
    { "taskmgr", "Task manager", "Processes, CPU and memory" },
    { "control", "Control panel", "Theme, keyboard, mouse, power" },
    { "devmgr",  "Device manager", "What is in this machine" },
    { "sh",      "Terminal",     "The command shell" },
    { "play",    "Music",        "Play a WAV file" },
    { "doom",    "DOOM",         "It runs DOOM" },
    { "plasma",  "Plasma",       "A graphics demo" },
    { "python",  "Python",       "An interactive prompt, or run a script" },
    { "cc",      "C compiler",   "Build a program on the machine itself" },
};
#define N_KNOWN ((int)(sizeof start_known / sizeof start_known[0]))

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void str_cpy(char *d, const char *s2, int max) {
    int i = 0;
    while (s2[i] && i < max - 1) { d[i] = s2[i]; i++; }
    d[i] = 0;
}

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive substring, which is what a person means by "search".
static int str_has(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && lower_c(hay[i + k]) == lower_c(needle[k])) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

static void start_add(const char *file, int featured) {
    if (start_count >= START_MAX) return;
    for (int i = 0; i < start_count; i++) {
        // Already listed by its friendly name: do not list it twice.
        int j = 0;
        while (start_items[i].path[j]) j++;
        while (j > 0 && start_items[i].path[j - 1] != '/') j--;
        if (str_eq(start_items[i].path + j, file)) return;
    }
    struct start_item *it = &start_items[start_count++];
    it->featured = featured;
    str_cpy(it->name, file, START_NAME);
    str_cpy(it->name_file, file, START_NAME);
    it->desc[0] = 0;
    int n = 0;
    const char *pre = "/bin/";
    while (pre[n]) { it->path[n] = pre[n]; n++; }
    int k = 0;
    while (file[k] && n < 46) it->path[n++] = file[k++];
    it->path[n] = 0;
}

static void start_build(void) {
    start_count = 0;
    for (int i = 0; i < N_KNOWN; i++) {
        struct start_item *it = &start_items[start_count++];
        it->featured = 1;
        str_cpy(it->name, start_known[i].name, START_NAME);
        str_cpy(it->name_file, start_known[i].file, START_NAME);
        str_cpy(it->desc, start_known[i].desc, START_DESC);
        str_cpy(it->path, "/bin/", 48);
        int n = 5, k = 0;
        while (start_known[i].file[k] && n < 46) it->path[n++] = start_known[i].file[k++];
        it->path[n] = 0;
    }

    static char buf[4096];
    uint32_t n = vfs_list_dir("/bin", buf, sizeof buf);
    char name[64];
    for (uint32_t i = 0; i < n; ) {
        uint32_t j = i;
        while (j < n && buf[j] != '\n') j++;
        int len = (int)(j - i);
        if (len > 0 && buf[j - 1] == '/') len--;          // a directory
        else if (len > 0) {
            if (len > 63) len = 63;
            for (int k = 0; k < len; k++) name[k] = buf[i + k];
            name[len] = 0;
            start_add(name, 0);
        }
        i = j + 1;
    }
}

// Index of the `want`-th item matching the current query, or -1.
static int start_nth(int want) {
    int seen = 0;
    for (int i = 0; i < start_count; i++) {
        if (!str_has(start_items[i].name, start_q) &&
            !str_has(start_items[i].path, start_q)) continue;
        if (seen == want) return i;
        seen++;
    }
    return -1;
}

static int start_matches(void) {
    int n = 0;
    while (start_nth(n) >= 0) n++;
    return n;
}

#define SM_W       420
#define SM_ROW_PAD 10

static int sm_row_h(void) { return (int)GH + SM_ROW_PAD; }

static void start_geom(int *x, int *y, int *w, int *h) {
    int H = (int)fb_get_height();
    int rows = 14;          // one line each now, so the list can be longer
    *h = rows * sm_row_h() + (int)GH + 30 + 34;
    *w = SM_W;
    *x = 8;
    *y = H - TASKBAR_H - *h;
    if (*y < 4) { *y = 4; *h = H - TASKBAR_H - 4; }
}

static void draw_start_menu(void) {
    if (!start_open) return;
    const theme_t *T = TH;
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || (uint32_t)yy >= fb_get_height()) continue;
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || (uint32_t)xx >= fb_get_width()) continue;
            row[xx] = mix(row[xx], T->menu_bg, 205);
        }
    }
    fb_fill_rect(x, y, w, 2, T->glow);
    fb_fill_rect(x, y + h - 2, w, 2, T->glow);
    fb_fill_rect(x, y, 2, h, T->glow);
    fb_fill_rect(x + w - 2, y, 2, h, T->glow);

    draw_text_t(x + 14, y + 12, "Programs", T->bar_dim);
    fb_fill_rect(x + 12, y + 12 + (int)GH + 6, w - 24, 1, mix(T->glow, 0x00000000, 120));

    int rh = sm_row_h();
    int list_y = y + 12 + (int)GH + 14;
    int list_h = h - (list_y - y) - ((int)GH + 30);
    int vis = list_h / rh;
    if (vis < 1) vis = 1;

    int total = start_matches();
    if (start_sel >= total) start_sel = total - 1;
    if (start_sel < 0) start_sel = total ? 0 : -1;
    if (start_sel >= 0) {
        if (start_sel < start_top) start_top = start_sel;
        if (start_sel >= start_top + vis) start_top = start_sel - vis + 1;
    }
    if (start_top > total - vis) start_top = total - vis;
    if (start_top < 0) start_top = 0;

    for (int r = 0; r < vis; r++) {
        int idx = start_top + r;
        int it = start_nth(idx);
        if (it < 0) break;
        int ry = list_y + r * rh;
        if (idx == start_sel) {
            fill_vgrad(x + 8, ry, w - 16, rh, mix(T->glow, 0x00FFFFFF, 70), T->glow);
            fb_fill_rect(x + 8, ry, w - 16, 1, T->title_hl);
        }
        // The icon is looked up by the program's FILE name, not its display
        // name: "Task manager" is what a person reads, taskmgr.png is what was
        // drawn for it. Small, because a row is one line high.
        int tx0 = x + 18;
        if (icon_draw(x + 14, ry + (rh - ICON_SMALL) / 2,
                      start_items[it].name_file, ICON_SMALL))
            tx0 = x + 14 + ICON_SMALL + 10;
        draw_text_t(tx0, ry + (rh - (int)GH) / 2, start_items[it].name,
                    idx == start_sel ? 0x00FFFFFF : T->bar_text);
    }
    if (!total)
        draw_text_t(x + 18, list_y + 6, "No program matches.", T->bar_dim);

    // The search box lives at the BOTTOM, next to the button that opened the
    // menu -- the pointer is already down there.
    int by = y + h - (int)GH - 22;
    fb_fill_rect(x + 10, by - 6, w - 20, (int)GH + 14, mix(T->bar_a, 0x00000000, 90));
    fb_fill_rect(x + 10, by - 6, w - 20, 1, T->glow);
    char shown[32];
    int i = 0;
    while (start_q[i] && i < 27) { shown[i] = start_q[i]; i++; }
    shown[i] = ((pit_get_ticks() / 50) & 1) ? '_' : ' ';
    shown[i + 1] = 0;
    draw_text_t(x + 18, by, start_qlen ? shown : "Search programs...",
                start_qlen ? 0x00FFFFFF : T->bar_dim);
}

static void draw_alttab(void) {
    if (!alttab_active) return;
    const theme_t *T = TH;
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    if (n <= 1) return;

    int rowh = (int)GH + 10;
    int bw = 380, bh = n * rowh + 24;
    int bx = ((int)fb_get_width() - bw) / 2;
    int by = ((int)fb_get_height() - bh) / 2;

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (int yy = by; yy < by + bh; yy++) {
        if (yy < 0 || (uint32_t)yy >= fb_get_height()) continue;
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (int xx = bx; xx < bx + bw; xx++) {
            if (xx < 0 || (uint32_t)xx >= fb_get_width()) continue;
            row[xx] = mix(row[xx], 0x00000000, 150);
        }
    }
    fb_fill_rect(bx, by, bw, 2, T->glow);
    fb_fill_rect(bx, by + bh - 2, bw, 2, T->glow);
    fb_fill_rect(bx, by, 2, bh, T->glow);
    fb_fill_rect(bx + bw - 2, by, 2, bh, T->glow);

    for (int i = 0; i < n; i++) {
        int ry = by + 12 + i * rowh;
        int sel = (win[i] == focused);
        if (sel) fill_vgrad(bx + 6, ry - 3, bw - 12, rowh,
                            mix(T->glow, 0x00FFFFFF, 60), T->glow);
        const char *t = pane_title(&panes[nodes[win[i]].pane_idx]);
        draw_text_t(bx + 18, ry, t, sel ? 0x00FFFFFF : T->bar_text);
        if (nodes[win[i]].state == WIN_MIN)
            draw_text_t(bx + bw - 110, ry, "minimised", T->bar_dim);
    }
    fb_mark_rows((uint32_t)(by < 0 ? 0 : by), (uint32_t)bh);
}

// The desktop underneath the arrow, saved before it is stamped on. Restoring
// this at the start of the next frame is what keeps a moving pointer from
// smearing a trail -- and it costs a couple of hundred pixels instead of the
// full-screen repaint that treating the cursor as "damage" would force.
static uint32_t cursor_save[CUR_W * CUR_H];
static int cur_saved_x = -1, cur_saved_y = -1;

static void cursor_restore(void) {
    if (cur_saved_x < 0) return;
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch(), fw = fb_get_width(), fh = fb_get_height();
    for (int r = 0; r < CUR_H; r++) {
        uint32_t py = (uint32_t)(cur_saved_y + r);
        if (py >= fh) break;
        volatile uint32_t *drow = (volatile uint32_t *)(base + (size_t)py * pitch);
        for (int c = 0; c < CUR_W; c++) {
            uint32_t px = (uint32_t)(cur_saved_x + c);
            if (px >= fw) break;
            drow[px] = cursor_save[r * CUR_W + c];
        }
    }
    fb_mark_rect((uint32_t)cur_saved_x, (uint32_t)cur_saved_y, CUR_W, CUR_H);
    cur_saved_x = -1;
}

static void cursor_draw(void) {
    if (!mouse_present()) return;
    int32_t mx, my;
    mouse_position(&mx, &my);

    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch(), fw = fb_get_width(), fh = fb_get_height();

    for (int r = 0; r < CUR_H; r++) {
        uint32_t py = (uint32_t)(my + r);
        if (py >= fh) break;
        volatile uint32_t *drow = (volatile uint32_t *)(base + (size_t)py * pitch);
        const char *row = cursor_bits[r];
        for (int c = 0; c < CUR_W; c++) {
            uint32_t px = (uint32_t)(mx + c);
            if (px >= fw) break;
            cursor_save[r * CUR_W + c] = drow[px];       // remember, then stamp
            char b = row[c];
            if (b == 'X')      drow[px] = 0x00000000;    // outline
            else if (b == '.') drow[px] = 0x00FFFFFF;    // fill
        }
    }
    fb_mark_rect((uint32_t)mx, (uint32_t)my, CUR_W, CUR_H);
    cur_saved_x = mx;
    cur_saved_y = my;
}

// --- outline drag ----------------------------------------------------------
// Moving a window by putting it at every pointer position means recompositing
// the desktop for each step: the wallpaper where it used to be, the window
// itself, and everything it uncovered on the way. Measured, that is the most
// expensive thing this compositor ever does -- eighteen million cycles a frame
// against four hundred thousand for an idle desktop.
//
// So a drag moves an OUTLINE. Four thin strips follow the pointer, the window
// itself does not move until the button comes up, and the frame costs what the
// strips cost: about ten thousand pixels instead of two million. Every window
// manager did this before compositing was affordable, for exactly this reason.
//
// The strips save what they cover, the way the cursor does. That is what keeps
// them from smearing a trail without declaring the whole window as damage.
#define OL_T 3                                /* strip thickness */
#define OL_SAVE_MAX 24576                     /* 2*(1920+1080)*3, with room */
static int ol_active = 0;                     /* a drag outline is in flight */
static int ol_x, ol_y, ol_w, ol_h;            /* where it is now */
static int ol_shown = 0;                      /* ol_save below is valid */
static int ol_sx, ol_sy, ol_sw, ol_sh;        /* what ol_save covers */
static uint32_t ol_save[OL_SAVE_MAX];

// The four strips of a rectangle, or the whole thing if it is too small to
// have an inside.
static int ol_strips(int x, int y, int w, int h, int r[4][4]) {
    if (w < OL_T * 2 || h < OL_T * 2) {
        r[0][0] = x; r[0][1] = y; r[0][2] = w; r[0][3] = h;
        return 1;
    }
    r[0][0] = x;             r[0][1] = y;          r[0][2] = w;    r[0][3] = OL_T;
    r[1][0] = x;             r[1][1] = y+h-OL_T;   r[1][2] = w;    r[1][3] = OL_T;
    r[2][0] = x;             r[2][1] = y+OL_T;     r[2][2] = OL_T; r[2][3] = h-2*OL_T;
    r[3][0] = x+w-OL_T;      r[3][1] = y+OL_T;     r[3][2] = OL_T; r[3][3] = h-2*OL_T;
    return 4;
}

// Put back what the strips covered. The save index advances for off-screen
// pixels too, so save and restore stay in step whatever the clipping did.
static void outline_hide(void) {
    if (!ol_shown) return;
    ol_shown = 0;
    int r[4][4];
    int n = ol_strips(ol_sx, ol_sy, ol_sw, ol_sh, r);
    uint8_t *base = (uint8_t *)fb_get_base();
    uint32_t pitch = fb_get_pitch(), fw = fb_get_width(), fh = fb_get_height();
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < r[i][3]; j++) {
            int py = r[i][1] + j;
            uint32_t *row = (py >= 0 && (uint32_t)py < fh)
                          ? (uint32_t *)(base + (size_t)py * pitch) : 0;
            for (int c = 0; c < r[i][2]; c++) {
                int px = r[i][0] + c;
                if (k >= OL_SAVE_MAX) return;
                if (row && px >= 0 && (uint32_t)px < fw) row[px] = ol_save[k];
                k++;
            }
        }
        if (r[i][2] > 0 && r[i][3] > 0)
            fb_mark_rect((uint32_t)(r[i][0] < 0 ? 0 : r[i][0]),
                         (uint32_t)(r[i][1] < 0 ? 0 : r[i][1]),
                         (uint32_t)r[i][2], (uint32_t)r[i][3]);
    }
}

static void outline_show(void) {
    if (!ol_active || ol_shown) return;
    ol_sx = ol_x; ol_sy = ol_y; ol_sw = ol_w; ol_sh = ol_h;
    int r[4][4];
    int n = ol_strips(ol_sx, ol_sy, ol_sw, ol_sh, r);
    uint8_t *base = (uint8_t *)fb_get_base();
    uint32_t pitch = fb_get_pitch(), fw = fb_get_width(), fh = fb_get_height();
    uint32_t accent = TH->glow;
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < r[i][3]; j++) {
            int py = r[i][1] + j;
            uint32_t *row = (py >= 0 && (uint32_t)py < fh)
                          ? (uint32_t *)(base + (size_t)py * pitch) : 0;
            for (int c = 0; c < r[i][2]; c++) {
                int px = r[i][0] + c;
                if (k >= OL_SAVE_MAX) { ol_shown = 1; return; }
                if (row && px >= 0 && (uint32_t)px < fw) {
                    ol_save[k] = row[px];        // remember, then stamp
                    row[px] = accent;
                }
                k++;
            }
        }
        if (r[i][2] > 0 && r[i][3] > 0)
            fb_mark_rect((uint32_t)(r[i][0] < 0 ? 0 : r[i][0]),
                         (uint32_t)(r[i][1] < 0 ? 0 : r[i][1]),
                         (uint32_t)r[i][2], (uint32_t)r[i][3]);
    }
    ol_shown = 1;
}

// End the drag: the desktop goes back the way it was before the window really
// moves, so a stale save can never be painted over the new layout.
static void outline_off(void) {
    outline_hide();
    ol_active = 0;
}

// --- frame profiling -------------------------------------------------------
// Where a frame actually goes. Guessing at this is how you end up optimising
// the part that was already cheap.
static uint64_t pf_wall, pf_win, pf_chrome, pf_present, pf_total;
static uint64_t pf_bytes, pf_rows, pf_rects;
static int      pf_frames, pf_cheap;
static int      pf_on;
static uint64_t pf_ms0, pf_tsc0;

void wm_palette(struct ui_palette *out) {
    const theme_t *T = &THEMES[theme];
    out->win = T->ui_win;       out->panel = T->ui_panel;   out->alt = T->ui_alt;
    out->text = T->ui_text;     out->dim = T->ui_dim;       out->line = T->ui_line;
    out->edge = T->ui_edge;     out->accent = T->ui_accent;
    out->accent2 = T->ui_accent2;
    out->sel = T->ui_sel;       out->hot = T->ui_hot;       out->btn = T->ui_btn;
    out->btndn = T->ui_btndn;   out->bar = T->ui_bar;   out->bar2 = T->ui_bar2;
    out->warn = T->ui_warn;     out->good = T->ui_good;
}

int wm_theme_get(void) { return theme; }
int wm_theme_count(void) { return NTHEMES; }

const char *wm_theme_name(int i) {
    if (i < 0 || i >= NTHEMES) return "";
    return THEMES[i].name;
}

void wm_theme_set(int i) {
    if (i < 0 || i >= NTHEMES || i == theme) return;
    theme = i;
    relayout();
    refresh_leaves();
    wp_dirty = 1;
    dirty = 1;
}

int  wm_dblclick_ms(void) { return dblclick_ms; }
void wm_set_dblclick_ms(int ms) {
    if (ms < 150) ms = 150;
    if (ms > 1200) ms = 1200;
    dblclick_ms = ms;
}

void wm_profile(int on) {
    pf_on = on;
    pf_wall = pf_win = pf_chrome = pf_present = pf_total = 0;
    pf_bytes = pf_rows = pf_rects = 0;
    pf_frames = pf_cheap = 0;
}

static void pf_report(void);

// Overlays that blend with WHAT IS ALREADY IN THE BUFFER rather than painting
// over it: the snap preview's tint, the drag ghost's slab, the start menu's
// panel, the Alt+Tab list, the modal dim. Drawing any of them twice tints
// twice, so the frame underneath has to be rebuilt every time one is up.
//
// This used to hold by accident. Dragging a window called relayout() on every
// pointer event, relayout() sets wp_dirty, and so the desktop happened to be
// repainted under the snap preview on every frame. Moving a window as an
// outline removed that call -- correctly, it is the whole point -- and the
// preview started compounding instead: hold a window against an edge and the
// tint builds up until the screen is solid blue.
//
// So the rule is stated here rather than inherited from a side effect.
static int overlay_needs_repaint(void) {
    return wm_message_pending() || snap_hint != SNAP_NONE || udrag_active ||
           start_open || alttab_active;
}

static void render_all(void) {
    uint64_t t0 = pf_on ? rdtsc() : 0, t1, t2, t3, t4;
    sample_stats();
    cursor_restore();   // put back what the arrow covered last frame, first
    outline_hide();     // and the drag outline, which sits under the arrow
    // Kernel logging draws straight into the back buffer, and a wallpaper
    // repaint covers the whole screen (panes included), so both force the
    // cached layers to be painted again.
    if (fb_console_wrote()) wp_dirty = 1;
    if (overlay_needs_repaint()) wp_dirty = 1;   // see the note above
    if (wp_dirty) { invalidate_pane_cache(); tb_valid = 0; }
    draw_wallpaper();
    t1 = pf_on ? rdtsc() : 0;
    render_windows();
    t2 = pf_on ? rdtsc() : 0;
    if (monitor_on) draw_gadget();
    draw_snap_preview();
    draw_alttab();
    draw_start_menu();
    draw_taskbar();
    draw_drag_ghost();    // follows the cursor, above the windows
    draw_message_box();   // above everything: it is modal
    outline_show();     // the drag outline, above the windows it will land on
    cursor_draw();      // last, so the pointer floats above everything
    t3 = pf_on ? rdtsc() : 0;
    fb_present();

    if (!pf_on) return;
    t4 = rdtsc();
    pf_wall    += t1 - t0;
    pf_win     += t2 - t1;
    pf_chrome  += t3 - t2;
    pf_present += t4 - t3;
    pf_total   += t4 - t0;
    pf_bytes   += fb_last_bytes();
    pf_rows    += fb_last_rows();
    pf_rects   += fb_last_rects();
    pf_report();
}

// Every sixtieth frame, say where the frames went. Called from both the full
// render and the cheap pointer/outline path -- a profiler that goes silent
// during exactly the interaction you wanted to profile is worse than none.
static void pf_report(void) {
    if (++pf_frames >= 60) {
        // Wall clock, from the timer -- cycles alone cannot be compared
        // between an emulated CPU and an accelerated one.
        uint64_t now_ms = pit_get_ticks() * 10;
        unsigned span = (unsigned)(now_ms - pf_ms0);
        pf_ms0 = now_ms;
        // How fast this clock actually runs, so a per-frame cost can be
        // turned into a frame rate. Under emulation the TSC is virtual,
        // so measuring it against the timer is the only honest way to
        // compare an emulated CPU against an accelerated one.
        uint64_t tsc_now = rdtsc();
        unsigned mhz = span ? (unsigned)((tsc_now - pf_tsc0) / (span * 1000)) : 0;
        pf_tsc0 = tsc_now;
        unsigned percyc = (unsigned)(pf_total / pf_frames);
        kprintf("gfx: clock %u MHz, frame %u us -> %u fps ceiling\n",
                mhz, (percyc && mhz) ? percyc / mhz : 0,
                (percyc && mhz) ? (mhz * 1000000u) / percyc : 0);
        kprintf("gfx: %d frames avg kcyc total=%u wall=%u win=%u chrome=%u present=%u\n",
                pf_frames, (unsigned)(pf_total / pf_frames / 1000),
                (unsigned)(pf_wall / pf_frames / 1000),
                (unsigned)(pf_win / pf_frames / 1000),
                (unsigned)(pf_chrome / pf_frames / 1000),
                (unsigned)(pf_present / pf_frames / 1000));
        kprintf("gfx: avg rows=%u bytes=%u KiB rects=%u, delivered %u Hz\n",
                (unsigned)(pf_rows / pf_frames),
                (unsigned)(pf_bytes / pf_frames / 1024),
                (unsigned)(pf_rects / pf_frames),
                (unsigned)cur_fps);
        kprintf("gfx: of those, %u were cheap frames (pointer or outline only)\n",
                (unsigned)pf_cheap);
        kprintf("gfx: panes painted=%u skipped=%u fp=%u paint=%u content=%u\n",
                (unsigned)(pf_painted / pf_frames), (unsigned)(pf_skipped / pf_frames),
                (unsigned)(pf_fp / pf_frames / 1000),
                (unsigned)(pf_paint / pf_frames / 1000),
                (unsigned)(pf_content / pf_frames / 1000));
        if (pf_blits)
            kprintf("gfx: app blits=%u avg %u kcyc\n",
                    (unsigned)pf_blits, (unsigned)(pf_blit / pf_blits / 1000));
        pf_fp = pf_paint = pf_content = pf_blit = 0;
        pf_painted = pf_skipped = pf_blits = 0;
        pf_wall = pf_win = pf_chrome = pf_present = pf_total = 0;
        pf_bytes = pf_rows = pf_rects = 0;
        pf_cheap = 0;
        pf_frames = 0;
    }
}

// Public repaint hook for code that blocks in its own modal key loop (fm's
// name prompts and confirmations) and therefore isn't letting the WM event
// loop repaint between keystrokes. See wm.h.
void wm_refresh(void) {
    render_all();
}

// A full-screen TUI (fm, the editor) needs to repaint itself after the layout
// changes under it. The compositor cannot do that for it any more -- the
// content belongs to a userland process now -- so it delivers a synthetic
// resize keystroke and the program redraws in its own time.
static void refresh_leaves(void) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].ws != cur_ws) continue;
        struct pane *p = &panes[nodes[i].pane_idx];
        if (p->owner_pid > 0) {
            struct kbd_event ev = { KEY_RESIZE, 0, 0, 1 };
            pane_push_event(p, &ev);
            process_wake_key();
        }
    }
}

// --- leaf collection / focus navigation ---

// Windows of a workspace, back to front (ascending z). Minimised windows are
// included -- the taskbar still lists them -- so drawing callers skip WIN_MIN.
static int collect_windows(int ws, int *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_NODES && n < max; i++)
        if (nodes[i].used && nodes[i].ws == ws) out[n++] = i;
    for (int i = 1; i < n; i++) {            // insertion sort; n is tiny
        int v = out[i], j = i - 1;
        while (j >= 0 && nodes[out[j]].z > nodes[v].z) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

static int topmost_window(int ws) {
    int best = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].ws != ws) continue;
        if (nodes[i].state == WIN_MIN) continue;
        if (best < 0 || nodes[i].z > nodes[best].z) best = i;
    }
    return best;
}

static int ws_has_windows(int ws) {
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].ws == ws) return 1;
    return 0;
}

static void center_of(int n, int *cx, int *cy) {
    *cx = (int)nodes[n].x + (int)nodes[n].w / 2;
    *cy = (int)nodes[n].y + (int)nodes[n].h / 2;
}

static int iabs(int v) { return v < 0 ? -v : v; }

// Finds the leaf whose center is nearest to `from` in the given direction,
// or -1 if none. direction: KEY_LEFT/RIGHT/UP/DOWN.
static int find_leaf_dir(int from, int dir) {
    int leaves[MAX_PANES];
    int nc = collect_windows(cur_ws, leaves, MAX_PANES);
    if (nc <= 1) return -1;

    int fcx, fcy;
    center_of(from, &fcx, &fcy);

    int best = -1;
    int best_dist = 0x7fffffff;
    for (int i = 0; i < nc; i++) {
        int n = leaves[i];
        if (n == from) continue;
        int cx, cy;
        center_of(n, &cx, &cy);
        int in_dir = 0;
        switch (dir) {
            case KEY_LEFT:  in_dir = cx < fcx; break;
            case KEY_RIGHT: in_dir = cx > fcx; break;
            case KEY_UP:    in_dir = cy < fcy; break;
            case KEY_DOWN:  in_dir = cy > fcy; break;
        }
        if (!in_dir) continue;
        int primary, cross;
        if (dir == KEY_LEFT || dir == KEY_RIGHT) {
            primary = iabs(cx - fcx); cross = iabs(cy - fcy);
        } else {
            primary = iabs(cy - fcy); cross = iabs(cx - fcx);
        }
        int dist = primary + cross * 2;
        if (dist < best_dist) { best_dist = dist; best = n; }
    }
    return best;
}

static void focus_dir(int dir) {
    int target = find_leaf_dir(focused, dir);
    if (target >= 0) { focused = target; raise_window(target); }
}

// Nudge the focused window around the desktop.
static void move_dir(int dir) {
    if (focused < 0) return;
    struct wm_node *nd = &nodes[focused];
    if (nd->state == WIN_MAX) return;
    const uint32_t step = 40;
    switch (dir) {
        case KEY_LEFT:  nd->x = (nd->x > step) ? nd->x - step : 0; break;
        case KEY_RIGHT: nd->x += step; break;
        case KEY_UP:    nd->y = (nd->y > step) ? nd->y - step : 0; break;
        case KEY_DOWN:  nd->y += step; break;
    }
}

// Grow/shrink the focused window. RIGHT/DOWN grow, LEFT/UP shrink.
static void resize_dir(int dir) {
    if (focused < 0) return;
    struct wm_node *nd = &nodes[focused];
    if (nd->state == WIN_MAX) return;
    const uint32_t step = 40;
    switch (dir) {
        case KEY_LEFT:  if (nd->w > win_min_w() + step) nd->w -= step; break;
        case KEY_RIGHT: nd->w += step; break;
        case KEY_UP:    if (nd->h > win_min_h() + step) nd->h -= step; break;
        case KEY_DOWN:  nd->h += step; break;
    }
}

// --- window lifecycle -----------------------------------------------------

// Open a window, cascaded down-right from the last one the way a classic
// desktop does, and start a shell in it.
// Open a window running `path`. With a NULL path this is the plain "new
// terminal" case; with one it is a program launched from the start menu,
// whose window closes when it exits instead of falling back to a shell.
static int new_window_run(const char *path, const char *arg) {
    int pi = alloc_pane();
    int n  = alloc_node();
    if (pi < 0 || n < 0) {
        if (pi >= 0) panes[pi].alive = 0;    // hand the claim back
        free_node(n);
        return -1;
    }

    uint32_t dx, dy, dw, dh;
    desk_area(&dx, &dy, &dw, &dh);
    static int cascade = 0;
    uint32_t offs = (uint32_t)((cascade++ % 6) * 34);

    struct wm_node *nd = &nodes[n];
    nd->pane_idx = pi;
    nd->ws       = cur_ws;
    nd->state    = WIN_NORMAL;
    nd->w = dw * 62 / 100;
    nd->h = dh * 58 / 100;
    nd->x = dx + offs;
    nd->y = dy + offs;
    nd->z = ++z_top;

    focused = n;
    layout_window(n);
    pane_init(&panes[pi], icols(nd->w), irows(nd->h));
    layout_window(n);
    wp_dirty = 1;
    if (path) { panes[pi].app_pane = 1; spawn_prog_in(pi, path, arg); }
    else      { panes[pi].app_pane = 0; spawn_shell_in(pi); }
    return panes[pi].owner_pid;
}

static void new_window(void) { new_window_run(0, 0); }

// Open a window for a program on someone else's behalf.
//
// This is what makes the file manager behave like a file manager: opening a
// document has to produce a SEPARATE window, not hand the document to a
// program that then paints over the window you launched it from. A plain
// spawn() cannot do that -- a child inherits its parent's pane, which is
// right for `grep | sort` in a terminal and wrong for double-clicking a file.
//
// The path is checked first so a mistyped or missing program does not leave
// an empty window standing on the desktop.
int wm_spawn_window(const char *path, const char *arg) {
    // Copy the strings out of the caller's address space FIRST.
    //
    // They are user pointers, valid only while the caller's page tables are
    // loaded -- and building the child swaps in a different address space
    // before the ELF loader gets to look at argv. Passing them through
    // unchanged reads whatever now lives at those addresses, which is how the
    // editor came to open a directory instead of the file that was clicked.
    static char kpath[128], karg[256];
    int i = 0;
    if (!path) return -1;
    while (path[i] && i < (int)sizeof kpath - 1) { kpath[i] = path[i]; i++; }
    kpath[i] = 0;

    int have_arg = arg != 0;
    i = 0;
    if (have_arg) {
        while (arg[i] && i < (int)sizeof karg - 1) { karg[i] = arg[i]; i++; }
    }
    karg[i] = 0;

    struct vfs_stat st;
    if (vfs_stat(kpath, &st) != 0 || st.is_dir) return -1;

    int pid = new_window_run(kpath, have_arg ? karg : 0);
    refresh_leaves();
    dirty = 1;
    return pid;
}

// Alt+F4 / the close button: end whatever is in this window.
//
// "Whatever is in this window" is the subtle part. A program started from the
// shell does not get a window of its own -- it takes over the terminal's
// pane. Closing then has to mean "close the program", leaving the terminal it
// was launched from; only a window running nothing but its own shell is
// closed outright. Without that distinction a full-screen program started
// from a prompt has no way out at all, because refusing to close the last
// window refuses to close the program too.
static void close_focused(void) {
    if (focused < 0) return;
    int pi = nodes[focused].pane_idx;
    int owner = panes[pi].owner_pid;

    if (owner > 0) {
        int fg = process_foreground(owner);
        if (fg > 0 && fg != owner) {
            process_kill(fg);            // a program inside the terminal
            return;
        }
    }

    int win[MAX_NODES];
    if (collect_windows(cur_ws, win, MAX_NODES) <= 1) return;  // keep one open

    // The window really is going. Kill what it was running, or the process
    // survives with nowhere left to draw: an orphan that keeps being
    // scheduled and that only the task manager can even see.
    if (owner > 0) process_kill(owner);
    panes[pi].alive = 0;
    if (panes[pi].gfx) { kfree(panes[pi].gfx); panes[pi].gfx = 0; }
    panes[pi].gfx_on = 0;
    panes[pi].gfx_w = panes[pi].gfx_h = 0;
    panes[pi].gfx_pid = 0;
    free_node(focused);
    focused = topmost_window(cur_ws);
    relayout();
}

// Snap the focused window to half the desktop, the way dragging a window to a
// screen edge does on Windows.
static void snap_dir(int dir) {
    if (focused < 0) return;
    struct wm_node *nd = &nodes[focused];
    uint32_t dx, dy, dw, dh;
    desk_area(&dx, &dy, &dw, &dh);

    if (nd->state == WIN_MAX) nd->state = WIN_NORMAL;
    if (dir == KEY_LEFT || dir == KEY_RIGHT) {
        nd->y = dy; nd->h = dh; nd->w = dw / 2;
        nd->x = (dir == KEY_LEFT) ? dx : dx + dw - nd->w;
    } else {
        return;
    }
    raise_window(focused);
    relayout();
}

static void minimize_focused(void) {
    if (focused < 0) return;
    nodes[focused].state = WIN_MIN;
    focused = topmost_window(cur_ws);
    relayout();
}

static void restore_window(int n) {
    if (n < 0 || !nodes[n].used) return;
    if (nodes[n].state == WIN_MIN) nodes[n].state = WIN_NORMAL;
    raise_window(n);
    focused = n;
    relayout();
}

// Alt+Tab: step through this workspace's windows, minimised ones included --
// picking one restores it, exactly as it does on Windows.
static void alt_tab(int back) {
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    if (n <= 1) return;
    int cur = 0;
    for (int i = 0; i < n; i++) if (win[i] == focused) cur = i;
    int next = back ? (cur - 1 + n) % n : (cur + 1) % n;
    restore_window(win[next]);
}

// Win+D: clear the desktop, then put everything back on a second press.
static void show_desktop(void) {
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    int any_visible = 0;
    for (int i = 0; i < n; i++) if (nodes[win[i]].state != WIN_MIN) any_visible = 1;
    for (int i = 0; i < n; i++)
        nodes[win[i]].state = any_visible ? WIN_MIN : WIN_NORMAL;
    focused = any_visible ? -1 : topmost_window(cur_ws);
    relayout();
}

// Maximise / restore the focused window. This is the old "back to a single
// pane" key, which on a floating desktop naturally means "fill the screen".
static void toggle_maximize(void) {
    if (focused < 0) return;
    struct wm_node *nd = &nodes[focused];
    if (nd->state == WIN_MAX) {
        nd->state = WIN_NORMAL;
        nd->x = nd->sx; nd->y = nd->sy; nd->w = nd->sw; nd->h = nd->sh;
    } else {
        nd->sx = nd->x; nd->sy = nd->y; nd->sw = nd->w; nd->sh = nd->h;
        nd->state = WIN_MAX;
    }
    raise_window(focused);
    relayout();
}

// --- pointer interaction ---------------------------------------------------
//
// Everything the mouse can grab, and what a press on it starts. Buttons live in
// the title bar at the right, resize handles along the frame edges.

#define HIT_NONE   0
#define HIT_TITLE  1
#define HIT_CLIENT 2
#define HIT_CLOSE  3
#define HIT_MAXBTN 4
#define HIT_MINBTN 5
#define HIT_L      0x10
#define HIT_R      0x20
#define HIT_T      0x40
#define HIT_B      0x80

static int hit_test(int n, int mx, int my) {
    const struct wm_node *nd = &nodes[n];
    int x0 = (int)nd->x, y0 = (int)nd->y;
    int x1 = x0 + (int)nd->w, y1 = y0 + (int)nd->h;
    if (mx < x0 || my < y0 || mx >= x1 || my >= y1) return HIT_NONE;

    if (nd->state != WIN_MAX) {              // resize handles on the frame
        int edge = 0;
        if (mx < x0 + GRAB) edge |= HIT_L;
        if (mx >= x1 - GRAB) edge |= HIT_R;
        if (my < y0 + GRAB) edge |= HIT_T;
        if (my >= y1 - GRAB) edge |= HIT_B;
        if (edge) return edge;
    }

    if (my < y0 + TITLE_H) {
        int by = y0 + (TITLE_H - 18) / 2;
        if (my >= by && my < by + 18) {
            for (int b = 0; b < 3; b++) {
                int bx = win_btn_x(nd, b);
                if (mx >= bx && mx < bx + BTN_W - 4)
                    return b == 0 ? HIT_MINBTN : (b == 1 ? HIT_MAXBTN : HIT_CLOSE);
            }
        }
        return HIT_TITLE;
    }
    return HIT_CLIENT;
}

// Topmost window under the pointer, or -1.
static int window_at(int mx, int my) {
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    for (int i = n - 1; i >= 0; i--) {       // front to back
        if (nodes[win[i]].state == WIN_MIN) continue;
        if (hit_test(win[i], mx, my) != HIT_NONE) return win[i];
    }
    return -1;
}

#define DRAG_NONE   0
#define DRAG_MOVE   1
#define DRAG_RESIZE 2
static int drag_mode = DRAG_NONE;

static int drag_win = -1, drag_edge = 0;
static int drag_gx, drag_gy;                 // where the grab started
static uint32_t drag_ox, drag_oy, drag_ow, drag_oh;   // geometry at grab time
static int press_hit = HIT_NONE, press_win = -1;      // for click-on-release

// Switch to workspace `n`, giving it a shell on first visit.
static void switch_ws(int n) {
    if (n < 0 || n >= MAX_WS || n == cur_ws) return;
    ws_focused[cur_ws] = focused;
    cur_ws = n;

    int top = topmost_window(cur_ws);
    if (top < 0) {
        focused = -1;
        new_window();                 // empty workspace: open one
    } else {
        int want = ws_focused[n];
        focused = (want >= 0 && nodes[want].used && nodes[want].ws == n) ? want : top;
        relayout();
        refresh_leaves();
    }
    dirty = 1;
}

// --- pane ownership -------------------------------------------------------

struct pane *wm_pane_for_pid(int pid) {
    if (pid <= 0) return 0;
    for (int i = 0; i < MAX_PANES; i++) {
        if (panes[i].alive && panes[i].owner_pid == pid) return &panes[i];
    }
    return 0;
}

static void pane_push_event(struct pane *p, const struct kbd_event *ev) {
    uint32_t next = (p->evq_head + 1) % PANE_EVQ_MAX;
    if (next == p->evq_tail) return;   // full: drop the oldest input, not the newest
    p->evq[p->evq_head] = *ev;
    p->evq_head = next;
}

int wm_pane_pop_event(struct pane *p, struct kbd_event *out) {
    if (p->evq_tail == p->evq_head) return 0;
    *out = p->evq[p->evq_tail];
    p->evq_tail = (p->evq_tail + 1) % PANE_EVQ_MAX;
    return 1;
}

int wm_pane_pop_mouse(struct pane *p, struct pane_mouse *out) {
    if (p->mq_tail == p->mq_head) return 0;
    *out = p->mq[p->mq_tail];
    p->mq_tail = (p->mq_tail + 1) % PANE_MOUSEQ_MAX;
    return 1;
}

// Hand a pointer event to the program owning window , in ITS coordinates.
// Motion with nothing held is dropped unless the program asked for hover, so
// an idle mouse drifting over a window does not wake a sleeping process.
static void pane_push_mouse_ex(int n, const struct mouse_event *me, int phase) {
    struct wm_node *nd = &nodes[n];
    if (nd->pane_idx < 0) return;
    struct pane *p = &panes[nd->pane_idx];
    if (!p->alive) return;

    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    if (!cw || !ch) return;

    int lx = me->x - (int)cx, ly = me->y - (int)cy;
    // A gfx surface is blitted SCALED into the interior, so undo that scale.
    if (p->gfx_on && p->gfx_w && p->gfx_h) {
        lx = lx * (int)p->gfx_w / (int)cw;
        ly = ly * (int)p->gfx_h / (int)ch;
    }

    struct pane_mouse *e = &p->mq[p->mq_head];
    e->x = (int16_t)lx;
    e->y = (int16_t)ly;
    e->buttons  = me->buttons;
    e->pressed  = me->pressed;
    e->released = me->released;
    e->wheel    = me->wheel;
    e->drag     = (uint8_t)phase;

    p->mq_head = (p->mq_head + 1) % PANE_MOUSEQ_MAX;
    if (p->mq_head == p->mq_tail) p->mq_tail = (p->mq_tail + 1) % PANE_MOUSEQ_MAX;
    if (p->owner_pid > 0) process_wake_key();
}

static void pane_push_mouse(int n, const struct mouse_event *me) {
    pane_push_mouse_ex(n, me, 0);
}

// Leave the payload where only the window it was dropped on can read it.
static void pane_deliver_drop(int n) {
    struct wm_node *nd = &nodes[n];
    if (nd->pane_idx < 0) return;
    struct pane *p = &panes[nd->pane_idx];
    if (!p->alive) return;
    int i = 0;
    while (udrag_payload[i] && i < PANE_DROP_MAX - 1) { p->drop[i] = udrag_payload[i]; i++; }
    p->drop[i] = 0;
    p->drop_ready = 1;
}

void wm_mark_dirty(void) { dirty = 1; }

// --- graphics-mode panes (raw pixel surfaces, e.g. DOOM) ------------------

static int node_for_pane(const struct pane *p) {
    int idx = (int)(p - panes);
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].pane_idx == idx) return i;
    return -1;
}

// The pane's content pixel size (below the title bar, inside the frame). 0 on
// failure. This is what a graphics program (DOOM) renders into.
void wm_pane_interior(struct pane *p, int *w, int *h) {
    int n = node_for_pane(p);
    if (n < 0) {
        kprintf("DBG interior: no node for pane %d\n", (int)(p - panes));
        *w = *h = 0;
        return;
    }
    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    if (!cw || !ch)
        kprintf("DBG interior: node %d state=%d geom %dx%d at %d,%d -> %dx%d\n",
                n, nodes[n].state, (int)nodes[n].w, (int)nodes[n].h,
                (int)nodes[n].x, (int)nodes[n].y, (int)cw, (int)ch);
    *w = (int)cw;
    *h = (int)ch;
}

// Repaint just this window's interior, when nothing else can be on top of it.
//
// A graphics program blits every frame it draws, and putting that through the
// full compositor means repainting the wallpaper, every other window and the
// taskbar in order to show one picture. That is what makes an image viewer
// feel slow and the desktop appear to flicker underneath it: the cost per
// frame is the whole screen several times over, when the only pixels that can
// possibly have changed are the ones inside this window.
//
// The shortcut is only valid while nothing overlaps those pixels, so anything
// that draws over the desktop -- an open menu, the Alt+Tab list, a message
// box, a snap preview, the monitor gadget -- disqualifies it, as does a
// window stacked above this one.
static int present_pane_only(int n) {
    if (n < 0 || !nodes[n].used) return 0;
    if (nodes[n].ws != cur_ws || nodes[n].state == WIN_MIN) return 0;
    if (start_open || alttab_active || monitor_on || udrag_active) return 0;
    // A program painting its own window would erase the strips without
    // restoring what they saved, and the save would then be garbage.
    if (ol_active) return 0;
    if (snap_hint != SNAP_NONE || wm_message_pending()) return 0;
    if (wp_dirty) return 0;                 // a full repaint is already owed

    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    if (!cw || !ch) return 0;

    for (int i = 0; i < MAX_NODES; i++) {
        if (i == n || !nodes[i].used) continue;
        if (nodes[i].ws != cur_ws || nodes[i].state == WIN_MIN) continue;
        if (nodes[i].z <= nodes[n].z) continue;
        if (nodes[i].x >= cx + cw || cx >= nodes[i].x + nodes[i].w) continue;
        if (nodes[i].y >= cy + ch || cy >= nodes[i].y + nodes[i].h) continue;
        return 0;                            // something is stacked over it
    }

    cursor_restore();
    draw_content(n, cx, cy, cw, ch, n == focused);
    cursor_draw();
    fb_present();
    return 1;
}

// Copy an ARGB frame into the pane, switch it to graphics mode, and
// composite+present immediately (a running game never hits the idle repaint).
int wm_pane_blit(struct pane *p, const uint32_t *src, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    uint32_t need = (uint32_t)w * (uint32_t)h;
    if (!p->gfx || p->gfx_w != w || p->gfx_h != h) {
        if (p->gfx) kfree(p->gfx);
        p->gfx = kmalloc(need * 4);
        if (!p->gfx) { p->gfx_on = 0; return -1; }
        p->gfx_w = (uint16_t)w;
        p->gfx_h = (uint16_t)h;
    }
    // Two pixels per store. This is a program's whole window, copied on every
    // frame it draws -- at 1180x580 that is 2.7 MB, and doing it a pixel at a
    // time costs more than the compositing that follows it.
    {
        uint64_t b0 = rdtsc();
        uint32_t pairs = need / 2;
        const uint64_t *s8 = (const uint64_t *)src;
        uint64_t *d8 = (uint64_t *)p->gfx;
        for (uint32_t k = 0; k < pairs; k++) d8[k] = s8[k];
        if (need & 1) p->gfx[need - 1] = src[need - 1];
        pf_blit += rdtsc() - b0;
        pf_blits++;
    }
    p->gfx_on = 1;
    {   // Whoever is painting owns the surface for as long as they live.
        process_t *me = process_current();
        p->gfx_pid = me ? me->pid : 0;
    }
    if (!present_pane_only(node_for_pane(p))) wm_refresh();
    return 0;
}

void wm_pane_gfx_end(struct pane *p) {
    p->gfx_on = 0;
    p->gfx_pid = 0;
    if (p->gfx) { kfree(p->gfx); p->gfx = 0; p->gfx_w = p->gfx_h = 0; }
    pane_clear(p);
    dirty = 1;
    wm_refresh();
}

static void spawn_prog_in(int pane_idx, const char *path, const char *arg) {
    // argv[0] is the bare program name, the way a shell would pass it.
    int cut = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/') cut = i + 1;
    const char *argv[2] = { path + cut, arg };
    int pid = process_spawn(path, arg ? 2 : 1, argv);
    if (pid > 0) {
        process_make_session_root(pid);
        panes[pane_idx].owner_pid = pid;
    } else {
        // Nothing started: do not leave an empty window behind.
        panes[pane_idx].app_pane = 0;
        spawn_shell_in(pane_idx);
    }
}

static void spawn_shell_in(int pane_idx) {
    const char *argv[] = { "sh" };
    int pid = process_spawn("/bin/sh", 1, argv);
    if (pid > 0) {
        // Each pane shell is its own session root, NOT a child of whatever ran
        // the split -- otherwise one pane's Ctrl+C could walk into another's.
        process_make_session_root(pid);
        panes[pane_idx].owner_pid = pid;
    }
}

void wm_notify_exit(int pid) {
    // A drag belongs to a program that is now gone; nothing will ever drop it.
    if (udrag_active) wm_drag_cancel();

    // A graphics program does not have to OWN a pane -- one started from the
    // shell is a child, and the pane still belongs to the shell. Its last
    // frame would then sit in the pane for ever, hiding a terminal that is
    // alive and taking input underneath. So drop graphics mode for whichever
    // pane this process was painting, whether or not it owned it.
    for (int i = 0; i < MAX_PANES; i++) {
        if (!panes[i].alive || !panes[i].gfx_on) continue;
        if (panes[i].gfx_pid != pid) continue;
        if (panes[i].gfx) { kfree(panes[i].gfx); panes[i].gfx = 0; }
        panes[i].gfx_on = 0;
        panes[i].gfx_w = panes[i].gfx_h = 0;
        panes[i].gfx_pid = 0;
        invalidate_pane_cache();
        wp_dirty = 1;
        dirty = 1;
    }

    // Likewise the whole-keyboard mode: the program that asked for it is gone,
    // so the pane goes back to plain typing whether or not that program owned
    // it. Leaving it on is what made the shell echo everything twice after
    // DOOM exited.
    for (int i = 0; i < MAX_PANES; i++) {
        if (!panes[i].alive || panes[i].keys_raw_pid != pid) continue;
        panes[i].keys_raw = 0;
        panes[i].keys_raw_pid = 0;
    }

    for (int i = 0; i < MAX_PANES; i++) {
        if (!panes[i].alive || panes[i].owner_pid != pid) continue;
        panes[i].owner_pid = 0;
        panes[i].keys_raw = 0;      // the next program starts with the default
        panes[i].keys_raw_pid = 0;
        // Release any graphics surface the program held.
        if (panes[i].gfx) { kfree(panes[i].gfx); panes[i].gfx = 0; }
        panes[i].gfx_on = 0; panes[i].gfx_w = panes[i].gfx_h = 0;
        // A pane whose program died is useless; give it a fresh shell rather
        // than leaving a dead rectangle on screen.
        if (panes[i].app_pane) {
            panes[i].app_pane = 0;
            int win[MAX_NODES];
            int nwin = collect_windows(cur_ws, win, MAX_NODES);
            for (int k = 0; k < MAX_NODES; k++) {
                if (!nodes[k].used || nodes[k].pane_idx != i) continue;
                if (nwin > 1) {                 // never leave the desktop bare
                    panes[i].alive = 0;
                    free_node(k);
                    if (focused == k) focused = topmost_window(cur_ws);
                    relayout();
                } else {
                    pane_clear(&panes[i]);
                    spawn_shell_in(i);
                }
                break;
            }
            wp_dirty = 1;
            dirty = 1;
            continue;
        }
        pane_clear(&panes[i]);
        spawn_shell_in(i);
        dirty = 1;
    }
}

// --- input routing (called from the keyboard IRQ) -------------------------

// WM key bindings cannot be acted on here: splitting a pane spawns a process,
// which reads the disk and allocates. So a binding is only RECORDED, and
// wm_poll() performs it from the idle path with interrupts on.
static volatile int pending_binding[16];
static volatile uint32_t pb_head = 0, pb_tail = 0;

static void push_binding(int code) {
    uint32_t next = (pb_head + 1) % 16;
    if (next == pb_tail) return;
    pending_binding[pb_head] = code;
    pb_head = next;
}

// Encoded WM actions.
#define WMB_SPLIT     1
#define WMB_CLOSE     2
#define WMB_COLLAPSE  3
#define WMB_THEME     4
#define WMB_MONITOR   5
#define WMB_FOCUS     0x10   /* | direction */
#define WMB_MOVE      0x20
#define WMB_RESIZE    0x30
#define WMB_WS        0x40   /* | workspace index 0..8 */
#define WMB_SNAP      0x50   /* | direction: Aero-style half-screen snap */
#define WMB_ALTTAB    0x60   /* | 1 = backwards */
#define WMB_MINIMIZE  0x70
#define WMB_SHOWDESK  0x80
#define WMB_LAUNCH    0x90   /* run the start menu's selection */
#define WMB_PROFILE   0xA0   /* toggle frame profiling */
#define WMB_BENCH     0xB0   /* measure frames actually delivered */

// Clicking the taskbar: the workspace pills, then the window buttons. The
// geometry here has to mirror draw_taskbar()'s, so both walk the lists the
// same way and step by the same widths.
static void taskbar_click(int mx, int my) {
    (void)my;
    int orb_r = (TASKBAR_H - 14) / 2;
    int ocx = 8 + orb_r;
    int bx = ocx + orb_r + 8 + 6 * (int)GW + 14;

    if (mx < bx - 10) {                       // the orb and its label
        start_open = !start_open;
        if (start_open) { start_build(); start_sel = 0; start_top = 0; }
        start_qlen = 0;
        start_q[0] = 0;
        wp_dirty = 1;
        return;
    }

    for (int i = 0; i < MAX_WS; i++) {
        if (i != cur_ws && !ws_has_windows(i)) continue;
        int pw = (int)GW + 12;
        if (mx >= bx && mx < bx + pw) { switch_ws(i); return; }
        bx += pw + 4;
    }
    bx += 10;

    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    for (int i = 0; i < n; i++) {
        const char *t = pane_title(&panes[nodes[win[i]].pane_idx]);
        int len = 0; while (t[len] && len < 12) len++;
        int bw = len * (int)GW + 20;
        if (mx >= bx && mx < bx + bw) {
            // Clicking the active window's button minimises it, as on Windows.
            if (win[i] == focused && nodes[win[i]].state != WIN_MIN) {
                focused = win[i];
                minimize_focused();
            } else {
                restore_window(win[i]);
            }
            refresh_leaves();
            return;
        }
        bx += bw + 6;
    }
}

// Act on one pointer event: press starts a drag or arms a button, motion
// carries a drag, release completes whatever the press began.
static void handle_mouse(const struct mouse_event *me) {
    int mx = me->x, my = me->y;
    uint32_t H = fb_get_height();
    int on_taskbar = (my >= (int)H - TASKBAR_H);

    // A drag in flight takes precedence over everything: no window may be
    // moved, focused or clicked while one is crossing the screen.
    if (udrag_active) {
        int n = window_at(mx, my);
        int over = (n >= 0 && hit_test(n, mx, my) == HIT_CLIENT);
        int cur = over ? n : -1;

        // Tell the window it just left, so it can drop its highlight.
        if (cur != udrag_over && udrag_over >= 0 && nodes[udrag_over].used)
            pane_push_mouse_ex(udrag_over, me, 3);
        udrag_over = cur;

        if (me->released & MOUSE_LEFT) {
            if (over) {
                pane_deliver_drop(n);
                pane_push_mouse_ex(n, me, 2);
                restore_window(n);        // the drop target comes forward
            }
            udrag_active = 0;
            udrag_over = -1;
        } else if (over) {
            pane_push_mouse_ex(n, me, 1);
        }
        wp_dirty = 1;                     // the ghost moved; repaint under it
        dirty = 1;
        return;
    }

    // Pointer motion and wheel over the focused window's interior belong to
    // the program, not to the WM. Button transitions fall through below --
    // those may start a drag or hit window chrome first.
    if (drag_mode == DRAG_NONE && !on_taskbar && !me->pressed && !me->released) {
        int n = window_at(mx, my);
        if (n >= 0 && n == focused && hit_test(n, mx, my) == HIT_CLIENT)
            pane_push_mouse(n, me);
    }

    // --- motion while dragging ---
    // The window does not move here. Only the outline does; the geometry is
    // committed on release. Nothing is repainted from this function -- wm_poll
    // steps the strips, which is the whole point of doing it this way.
    if (drag_mode != DRAG_NONE && (me->buttons & MOUSE_LEFT)) {
        if (drag_win < 0 || !nodes[drag_win].used) {   // it closed under us
            outline_off();
            drag_mode = DRAG_NONE;
            drag_win = -1;
            return;
        }
        int dx = mx - drag_gx, dy = my - drag_gy;
        int nx, ny, nw, nh;
        if (drag_mode == DRAG_MOVE) {
            nx = (int)drag_ox + dx; ny = (int)drag_oy + dy;
            nw = (int)drag_ow;      nh = (int)drag_oh;
            int hint = snap_from_pointer(mx, my);
            // The snap preview is desktop-wide, so it does need a real frame.
            if (hint != snap_hint) { snap_hint = hint; wp_dirty = 1; dirty = 1; }
        } else {
            nx = (int)drag_ox; ny = (int)drag_oy;
            nw = (int)drag_ow; nh = (int)drag_oh;
            int mnw = (int)win_min_w(), mnh = (int)win_min_h();
            if (drag_edge & HIT_L) { nx += dx; nw -= dx; if (nw < mnw) { nx -= mnw - nw; nw = mnw; } }
            if (drag_edge & HIT_R) { nw += dx; if (nw < mnw) nw = mnw; }
            if (drag_edge & HIT_T) { ny += dy; nh -= dy; if (nh < mnh) { ny -= mnh - nh; nh = mnh; } }
            if (drag_edge & HIT_B) { nh += dy; if (nh < mnh) nh = mnh; }
        }
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (dx || dy) ol_active = 1;
        ol_x = nx; ol_y = ny; ol_w = nw; ol_h = nh;
        return;
    }

    // --- right button ---
    // It never starts a drag and never hits window chrome, so it is delivered
    // straight to whatever is under it. Pressing it focuses the window first,
    // the way clicking a background window before its context menu appears
    // does everywhere else.
    if ((me->pressed | me->released) & MOUSE_RIGHT) {
        if (on_taskbar) return;
        int n = window_at(mx, my);
        if (n < 0) return;
        if (hit_test(n, mx, my) != HIT_CLIENT) return;
        if (me->pressed & MOUSE_RIGHT) restore_window(n);
        pane_push_mouse(n, me);
        return;
    }

    // --- press ---
    if (me->pressed & MOUSE_LEFT) {
        press_hit = HIT_NONE;
        press_win = -1;

        if (wm_message_pending()) {
            int bx, by, bw, bh;
            mbox_ok_rect(&bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh)
                mbox_dismiss();
            return;                          // modal: nothing else sees this
        }

        // The start menu is modal for the pointer: a click inside picks a
        // program, a click anywhere else dismisses it.
        if (start_open && !on_taskbar) {
            int sx, sy, sw, sh;
            start_geom(&sx, &sy, &sw, &sh);
            if (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh) {
                int rh = sm_row_h();
                int list_y = sy + 12 + (int)GH + 14;
                int list_h = sh - (list_y - sy) - ((int)GH + 30);
                if (my >= list_y && my < list_y + list_h) {
                    int r = (my - list_y) / rh;
                    if (start_nth(start_top + r) >= 0) {
                        start_sel = start_top + r;
                        push_binding(WMB_LAUNCH);
                    }
                }
                return;
            }
            start_open = 0;
            wp_dirty = 1;
            return;
        }

        if (on_taskbar) { press_hit = HIT_CLIENT; press_win = -2; return; }

        int n = window_at(mx, my);
        if (n < 0) return;                       // press on the bare desktop
        restore_window(n);                       // focus + raise, like Windows
        int hit = hit_test(n, mx, my);
        press_hit = hit; press_win = n;

        if (hit == HIT_CLIENT) { pane_push_mouse(n, me); return; }

        if (hit == HIT_TITLE || (hit & 0xF0)) {
            struct wm_node *nd = &nodes[n];
            drag_win = n;
            drag_gx = mx; drag_gy = my;
            drag_ox = nd->x; drag_oy = nd->y; drag_ow = nd->w; drag_oh = nd->h;
            if (hit == HIT_TITLE) {
                if (nd->state == WIN_MAX) return;   // a maximised window stays put
                drag_mode = DRAG_MOVE;
            } else {
                drag_mode = DRAG_RESIZE;
                drag_edge = hit;
            }
            // The outline starts where the window is, but stays invisible
            // until the pointer actually moves -- a click on the title bar
            // should not flash a frame.
            ol_x = (int)nd->x; ol_y = (int)nd->y;
            ol_w = (int)nd->w; ol_h = (int)nd->h;
            ol_active = 0;
        }
        return;
    }

    // --- release: a button only fires if the press and release agree ---
    if (me->released & MOUSE_LEFT) {
        if (drag_mode != DRAG_NONE) {
            int moved = ol_active;
            outline_off();          // clean desktop before the window moves
            struct wm_node *nd = (drag_win >= 0 && nodes[drag_win].used)
                               ? &nodes[drag_win] : 0;
            if (moved && nd) {
                if (drag_mode == DRAG_MOVE && snap_hint != SNAP_NONE) {
                    // Remember where the window was BEFORE the drag, so
                    // restoring it later puts it back where the user had it.
                    nd->sx = drag_ox; nd->sy = drag_oy;
                    nd->sw = drag_ow; nd->sh = drag_oh;
                    if (snap_hint == SNAP_MAX) {
                        nd->state = WIN_MAX;
                    } else {
                        nd->state = WIN_NORMAL;
                        snap_rect(snap_hint, &nd->x, &nd->y, &nd->w, &nd->h);
                    }
                } else {
                    // Where the outline ended up is where the window goes.
                    nd->x = (uint32_t)ol_x; nd->y = (uint32_t)ol_y;
                    nd->w = (uint32_t)ol_w; nd->h = (uint32_t)ol_h;
                }
                relayout();
                refresh_leaves();
            }
            snap_hint = SNAP_NONE;
            drag_mode = DRAG_NONE;
            drag_win = -1;
            wp_dirty = 1;
            dirty = 1;
            if (moved) return;
            // A press that never moved is a click, not a drag: fall through so
            // the title bar's double-click still reaches the code below.
        }
        if (press_hit == HIT_CLIENT && press_win >= 0 && nodes[press_win].used) {
            pane_push_mouse(press_win, me);
            press_win = -1; press_hit = HIT_NONE;
            return;
        }
        if (press_win == -2) { taskbar_click(mx, my); press_win = -1; return; }
        if (press_win >= 0 && nodes[press_win].used &&
            hit_test(press_win, mx, my) == press_hit) {
            focused = press_win;
            if (press_hit == HIT_TITLE) {
                uint64_t now = pit_get_ticks() * 10;
                if (last_click_win == press_win && now - last_click_ms < (uint64_t)dblclick_ms) {
                    toggle_maximize();
                    refresh_leaves();
                    last_click_win = -1;
                    press_win = -1; press_hit = HIT_NONE;
                    return;
                }
                last_click_ms = now;
                last_click_win = press_win;
            }
            switch (press_hit) {
                case HIT_CLOSE:  close_focused(); break;
                case HIT_MAXBTN: toggle_maximize(); break;
                case HIT_MINBTN: minimize_focused(); break;
                default: break;
            }
        }
        press_win = -1;
        press_hit = HIT_NONE;
    }
}

// --- how fast can it actually put frames out -------------------------------
// Counting frames over wall-clock time during ordinary use measures how often
// something ASKED for a repaint. This renderer is event driven and spends most
// of its life idle, so that number describes the mouse, not the machine. To
// find the real ceiling, stop waiting for events and draw flat out for a fixed
// number of timer ticks. The PIT runs at 100 Hz, so a hundred ticks is one
// second and the frame count is the rate.
static void bench_phase(const char *name, int mode, unsigned ticks) {
    uint64_t edge = pit_get_ticks();
    while (pit_get_ticks() == edge) { }          // start on a tick boundary
    uint64_t t0 = pit_get_ticks();
    unsigned frames = 0;
    uint32_t pushed0 = fb_frames_pushed();
    // Cycles as well as wall time. If the two disagree the thread was not
    // running, which is a completely different problem from a slow frame.
    uint64_t c0 = rdtsc(), cyc = 0;

    uint32_t save_x = 0;
    int save_ol = ol_x;
    int f = focused;
    if (mode == 1 && f >= 0 && nodes[f].used) save_x = nodes[f].x;

    while (pit_get_ticks() - t0 < ticks) {
        switch (mode) {
        case 0:                                  // the whole desktop, every frame
            wp_dirty = 1;
            render_all();
            break;
        case 1:                                  // a window moved, the old way
            if (f >= 0 && nodes[f].used) {
                nodes[f].x = save_x + (frames & 1);
                relayout();
            }
            wp_dirty = 1;
            render_all();
            break;
        case 2:                                  // a window moved, as an outline
            cursor_restore();
            outline_hide();
            ol_x = save_ol + (int)(frames & 1);
            outline_show();
            cursor_draw();
            fb_present();
            break;
        case 3:                                  // pure screen bandwidth
            fb_mark_rows(0, fb_get_height());
            fb_present();
            break;
        default: {                               // back buffer only, no screen
            // The same number of pixels written to ordinary RAM. Whatever the
            // difference against phase 3 is, that is what the screen costs.
            fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x00101820);
            fb_present_discard();
            break;
        }
        }
        frames++;
    }
    cyc = rdtsc() - c0;

    if (mode == 1 && f >= 0 && nodes[f].used) { nodes[f].x = save_x; relayout(); }
    if (mode == 2) ol_x = save_ol;

    unsigned ms = (unsigned)(pit_get_ticks() - t0) * 10;
    unsigned mhz = ms ? (unsigned)(cyc / ((uint64_t)ms * 1000)) : 0;
    kprintf("fps: %s: %u frames in %u ms = %u Hz, %u kcyc/frame at %u MHz\n",
            name, frames, ms, ms ? frames * 1000 / ms : 0,
            frames ? (unsigned)(cyc / frames / 1000) : 0, mhz);
    (void)pushed0;
}

static void wm_benchmark(void) {
    kprintf("fps: measuring frames DELIVERED, one second per phase\n");
    bench_phase("full desktop", 0, 100);
    bench_phase("window drag", 1, 100);

    int f = focused;
    if (f >= 0 && nodes[f].used) {
        ol_x = (int)nodes[f].x; ol_y = (int)nodes[f].y;
        ol_w = (int)nodes[f].w; ol_h = (int)nodes[f].h;
        ol_active = 1;
        wp_dirty = 1;
        render_all();                            // a clean frame with it drawn
        bench_phase("outline drag", 2, 100);
        outline_off();
    }
    bench_phase("screen push", 3, 100);
    bench_phase("ram fill", 4, 100);
    wp_dirty = 1;
    dirty = 1;
    kprintf("fps: done\n");
}

void wm_route_input(void) {
    struct kbd_event ev;
    while (keyboard_poll_event(&ev)) {
        // Key-up, and the modifier keys as keys, exist for programs that track
        // what is HELD. They never mean a shortcut -- releasing Alt+F4 must not
        // close a second window -- and they only reach a program that asked for
        // them. Everything below this point may assume a key going down.
        if (!ev.pressed || ev.code == KEY_SHIFT || ev.code == KEY_CTRL ||
            ev.code == KEY_ALT || ev.code == KEY_SUPER) {
            if (focused >= 0 && nodes[focused].used) {
                struct pane *fp = &panes[nodes[focused].pane_idx];
                if (fp->keys_raw) {
                    pane_push_event(fp, &ev);
                    if (fp->owner_pid > 0) process_wake_key();
                }
            }
            continue;
        }
        int super = ev.mods & KBD_MOD_SUPER;
        int shift = ev.mods & KBD_MOD_SHIFT;
        int alt   = ev.mods & KBD_MOD_ALT;
        int is_arrow = (ev.code == KEY_LEFT || ev.code == KEY_RIGHT ||
                        ev.code == KEY_UP || ev.code == KEY_DOWN);
        int ctrl = ev.mods & KBD_MOD_CTRL;

        // A modal dialog eats input. Enter, Escape and space all mean OK --
        // whichever one someone reaches for, the box should go away.
        if (wm_message_pending()) {
            if (ev.code == KEY_ENTER || ev.code == KEY_ESC ||
                (ev.code == KEY_CHAR && ev.ascii == ' ')) mbox_dismiss();
            continue;
        }

        // Ctrl+Esc opens the start menu, as it has on every Windows since 3.1.
        if (ev.code == KEY_ESC && ctrl) {
            start_open = !start_open;
            if (start_open) { start_build(); start_sel = 0; start_top = 0; }
            start_qlen = 0;
            start_q[0] = 0;
            wp_dirty = 1;
            dirty = 1;
            continue;
        }

        // While the menu is up it owns the keyboard: no program should
        // receive the letters someone is typing into a search box.
        //
        // Every key that reaches the menu changes what it shows -- a narrower
        // list, a moved highlight -- and the panel is translucent, painted
        // over whatever is behind it. Redrawing it without rebuilding the
        // background first leaves the previous rows showing through the new
        // ones. That was always true; it used to hide because the desktop
        // repainted constantly and each pass darkened the leftovers a little
        // more. Now that a frame only redraws what changed, the leftovers
        // simply stay, so the menu has to ask for its background back.
        if (start_open) {
            wp_dirty = 1;
            if (ev.code == KEY_ESC) {
                start_open = 0; wp_dirty = 1; dirty = 1;
            } else if (ev.code == KEY_ENTER) {
                push_binding(WMB_LAUNCH);
            } else if (ev.code == KEY_UP) {
                if (start_sel > 0) start_sel--;
                dirty = 1;
            } else if (ev.code == KEY_DOWN) {
                start_sel++; dirty = 1;
            } else if (ev.code == KEY_BKSP) {
                if (start_qlen) start_q[--start_qlen] = 0;
                start_sel = 0; start_top = 0; dirty = 1;
            } else if (ev.code == KEY_CHAR && (unsigned char)ev.ascii >= 32 &&
                       (unsigned char)ev.ascii < 127) {
                if (start_qlen < (int)sizeof start_q - 1) {
                    start_q[start_qlen++] = ev.ascii;
                    start_q[start_qlen] = 0;
                }
                start_sel = 0; start_top = 0; dirty = 1;
            }
            continue;
        }

        // --- Windows-style window management ---
        if (ev.code == KEY_CHAR && ev.ascii == '	' && alt) {      // Alt+Tab
            push_binding(WMB_ALTTAB | (shift ? 1 : 0)); continue;
        }
        if (ev.code == KEY_F4 && alt) { push_binding(WMB_CLOSE); continue; }
        if (ev.code == KEY_CHAR && (ev.ascii == 'p' || ev.ascii == 'P') && super) {
            push_binding(WMB_PROFILE);
            continue;
        }
        if (ev.code == KEY_CHAR && (ev.ascii == 'b' || ev.ascii == 'B') && super) {
            push_binding(WMB_BENCH);
            continue;
        }
        if (ev.code == KEY_UP    && super && !shift) { push_binding(WMB_COLLAPSE); continue; }
        if (ev.code == KEY_DOWN  && super && !shift) { push_binding(WMB_MINIMIZE); continue; }
        if (ev.code == KEY_LEFT  && super && !shift) { push_binding(WMB_SNAP | KEY_LEFT); continue; }
        if (ev.code == KEY_RIGHT && super && !shift) { push_binding(WMB_SNAP | KEY_RIGHT); continue; }
        if (ev.code == KEY_CHAR && (ev.ascii == 'D' || ev.ascii == 'd') &&
            super) { push_binding(WMB_SHOWDESK); continue; }

        if (ev.code == KEY_CHAR && (ev.ascii == 'E' || ev.ascii == 'e') &&
            super && shift) { push_binding(WMB_COLLAPSE); continue; }
        if (ev.code == KEY_ENTER && super && shift) {
            push_binding(WMB_SPLIT); continue;
        }
        if (ev.code == KEY_CHAR && (ev.ascii == 'Q' || ev.ascii == 'q') &&
            super && shift) { push_binding(WMB_CLOSE); continue; }
        if (ev.code == KEY_CHAR && (ev.ascii == 'T' || ev.ascii == 't') &&
            super) { push_binding(WMB_THEME); continue; }
        if (ev.code == KEY_CHAR && (ev.ascii == 'M' || ev.ascii == 'm') &&
            super) { push_binding(WMB_MONITOR); continue; }
        // Super+1..9 switches virtual workspace (created on first visit).
        if (ev.code == KEY_CHAR && super && ev.ascii >= '1' && ev.ascii <= '9') {
            push_binding(WMB_WS | (ev.ascii - '1')); continue;
        }
        if (is_arrow && alt && shift)   { push_binding(WMB_RESIZE | ev.code); continue; }
        if (is_arrow && super && shift) { push_binding(WMB_MOVE   | ev.code); continue; }

        if (focused < 0) continue;
        struct pane *p = &panes[nodes[focused].pane_idx];

        // Scrollback review: Shift+PageUp/PageDown page the focused pane's
        // history. Pure rendering, so it is done here rather than deferred to a
        // WM binding (no process is spawned). Half a screen per press.
        if ((ev.code == KEY_PGUP || ev.code == KEY_PGDN) && shift) {
            int step = (int)(p->rows / 2);
            if (step < 1) step = 1;
            pane_scroll(p, ev.code == KEY_PGUP ? step : -step);
            dirty = 1;
            continue;
        }

        // Ctrl+C interrupts the FOREGROUND process -- the deepest descendant of
        // the pane's owner, i.e. whatever the shell is currently waiting on.
        // If the owner has no descendant (a shell sitting at its prompt), fall
        // through and deliver the key so the shell can cancel its own line
        // instead of the whole pane vanishing.
        // ...but only for a text program. A graphics-mode program is a
        // window, and in a window Ctrl+C means whatever that program says it
        // means -- in the editor, copy. Killing it from under the user
        // because the keystroke looks like a terminal interrupt is the kind
        // of bug that makes an editor "randomly close". Alt+F4 and the close
        // button are how a window is ended.
        if (ev.code == KEY_CHAR && (ev.mods & KBD_MOD_CTRL) &&
            (ev.ascii == 'c' || ev.ascii == 'C') && !p->gfx_on) {
            if (p->owner_pid > 0) {
                int fg = process_foreground(p->owner_pid);
                // SIGINT, not a bare kill: a program is entitled to catch
                // Ctrl+C and put its own things away. One that does not still
                // ends with 130, exactly as before.
                if (fg != p->owner_pid) { signal_send(fg, SIGINT); continue; }
            }
        }

        // Any real input jumps back to the live bottom -- you type, you see the
        // prompt. (Modifier-only auto-repeats carry no code, but those never
        // reach here.)
        if (p->scroll != 0) { pane_scroll_reset(p); dirty = 1; }

        // Ordinary input belongs to whoever owns the focused pane.
        pane_push_event(p, &ev);
        if (p->owner_pid > 0) process_wake_key();
    }

}

// --- the compositor tick (called from the scheduler's idle path) ----------

static void do_binding(int b) {
    int dir = b & 0x0F;
    switch (b & 0xF0) {
        case WMB_FOCUS:  focus_dir(dir); dirty = 1; return;
        case WMB_MOVE:   move_dir(dir); relayout(); refresh_leaves(); dirty = 1; return;
        case WMB_RESIZE: resize_dir(dir); relayout(); refresh_leaves(); dirty = 1; return;
        case WMB_WS:     switch_ws(dir); return;
        case WMB_SNAP:   snap_dir(dir); refresh_leaves(); dirty = 1; return;
        case WMB_ALTTAB: alt_tab(dir); alttab_active = 1; refresh_leaves(); dirty = 1; return;
        case WMB_MINIMIZE: minimize_focused(); dirty = 1; return;
        case WMB_SHOWDESK: show_desktop(); dirty = 1; return;
    }
    switch (b) {
        case WMB_SPLIT:    new_window(); refresh_leaves(); dirty = 1; break;
        case WMB_CLOSE:    close_focused(); refresh_leaves(); dirty = 1; break;
        case WMB_COLLAPSE: toggle_maximize(); refresh_leaves(); dirty = 1; break;
        case WMB_THEME:
            theme = (theme + 1) % NTHEMES;
            relayout(); refresh_leaves(); dirty = 1; break;
        case WMB_LAUNCH: {
            int it = start_nth(start_sel);
            start_open = 0;
            start_qlen = 0;
            start_q[0] = 0;
            wp_dirty = 1;
            if (it >= 0) new_window_run(start_items[it].path, 0);
            refresh_leaves();
            dirty = 1;
            break;
        }
        case WMB_PROFILE: {
            static int on;
            on = !on;
            wm_profile(on);
            kprintf("gfx: profiling %s\n", on ? "on" : "off");
            break;
        }
        case WMB_BENCH: wm_benchmark(); break;
        case WMB_MONITOR:
            // Hiding the gadget uncovers whatever it was drawn over.
            monitor_on = !monitor_on; wp_dirty = 1; dirty = 1; break;
    }
}

// Set while the screen is deliberately blank (SYS_POWER sleep). The screen is
// someone else's until it is cleared -- with several cores, the idle core that
// runs this would otherwise repaint the desktop over the black the moment it
// had nothing better to do.
static volatile int screen_asleep = 0;

void wm_set_asleep(int on) { screen_asleep = on; }

void wm_poll(void) {
    if (screen_asleep) return;

    // The pointer is drained here rather than in wm_route_input(): that runs
    // off the keyboard IRQ, so with no typing the mouse would never be read.
    struct mouse_event me;
    int pointer_moved = 0;
    while (mouse_poll_event(&me)) {
        // A button or a wheel can change anything, so those still owe a frame.
        // Bare motion changes only where the pointer and the drag outline are,
        // and both of those know how to move themselves.
        if (me.pressed || me.released || me.wheel) dirty = 1;
        else pointer_moved = 1;
        handle_mouse(&me);
    }

    keyboard_repeat_tick();     // hold a key long enough and it repeats

    // A program that tracks held keys has to be told when it stops receiving
    // them. It saw the key go down; if focus moves elsewhere it will never see
    // it come up, and it would keep walking forever.
    {
        static int last_focus = -1;
        if (focused != last_focus) {
            if (last_focus >= 0 && nodes[last_focus].used) {
                struct pane *lp = &panes[nodes[last_focus].pane_idx];
                if (lp->keys_raw) {
                    struct kbd_event fo = { KEY_FOCUSOUT, 0, 0, 1 };
                    pane_push_event(lp, &fo);
                    if (lp->owner_pid > 0) process_wake_key();
                }
            }
            last_focus = focused;
        }
    }

    // The Alt+Tab list stands until Alt is let go.
    if (alttab_active && !(keyboard_mods() & KBD_MOD_ALT)) {
        alttab_active = 0;
        wp_dirty = 1;
        dirty = 1;
    }
    if (!started) return;

    // Tick the clock ~once a second (PIT is 100 Hz) and repaint so it stays live
    // even when nothing else is happening.
    // Refresh the clock + monitor history ~once a second and repaint so the
    // desktop stays live even when nothing else is happening.
    if (sample_stats()) dirty = 1;

    while (pb_tail != pb_head) {
        int b = pending_binding[pb_tail];
        pb_tail = (pb_tail + 1) % 16;
        do_binding(b);
    }

    if (split_request) {
        split_request = 0;
        new_window();
        refresh_leaves();
        dirty = 1;
    }

    if (dirty) {
        dirty = 0;
        render_all();
    } else if (pointer_moved && overlay_needs_repaint()) {
        // An overlay that tints what is under it is on screen, so this frame
        // has to be a real one -- the cheap path would leave the tint to
        // accumulate.
        render_all();
    } else if (pointer_moved && !wp_dirty) {
        uint64_t c0 = pf_on ? rdtsc() : 0;
        // The cheap frame: nothing changed but where the pointer and the drag
        // outline are, and both saved what they covered. Putting that back and
        // stamping them at the new place IS the whole frame -- no wallpaper, no
        // windows, no taskbar. About ten thousand pixels instead of a million.
        cursor_restore();
        outline_hide();
        outline_show();
        cursor_draw();
        fb_present();
        if (pf_on) {
            pf_total += rdtsc() - c0;
            pf_bytes += fb_last_bytes();
            pf_rows  += fb_last_rows();
            pf_rects += fb_last_rects();
            pf_cheap++;
            pf_report();
        }
    }
}

void wm_start(void) {
    keyboard_flush();
    icons_load();             // /icons.bin, if the build produced one
    fb_enable_backbuffer();   // flicker-free: draw off-screen, blit per frame
    mouse_init(fb_get_width(), fb_get_height());
    kprintf("wm: double-buffer %s\n", fb_backbuffer_active() ? "ON" : "OFF (direct)");
    rtc_read(&wm_clock);      // seed the clock so it shows the right time at once

    for (int i = 0; i < MAX_PANES; i++) panes[i].alive = 0;
    for (int i = 0; i < MAX_NODES; i++) nodes[i].used = 0;
    for (int i = 0; i < MAX_WS; i++) ws_focused[i] = -1;
    cur_ws  = 0;
    z_top   = 0;
    focused = -1;

    started = 1;
    dirty = 1;
    new_window();          // the first window, holding the first shell
}
