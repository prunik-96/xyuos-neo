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
#include "../fs/vol.h"
#include "../kernel/clip.h"
#include "../kernel/kio.h"
#include "../drivers/mouse.h"
#include "../drivers/audio.h"
#include "../drivers/xhci.h"
#include "../drivers/power.h"
#include "../net/net.h"
#include "ui.h"
#include "wall.h"
#include "cursor.h"
#include "glyphs.h"
#include "splash.h"

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
//  The look: calm, soft, coloured glass. Windows are rounded cards whose top
//  is a strip of tabs; the panel is an island floating above the bottom
//  edge; the system's colour is taken from the wallpaper.
// ==========================================================================
#define TASKBAR_H  92      // the band the island floats in: 14 + 64 + 14
#define ISLAND_H   64      // the island itself
#define TITLE_H    44      // a window's strip of tabs
#define FRAME      6       // the glass at a window's sides and bottom
#define TPAD       8       // inner padding inside a terminal
#define AGAP       8       // gap of wallpaper between windows
#define RAD_T      18      // a window's corners
#define RAD_B      18
#define BODY_R     12      // the corners of what is inside it
#define SHADOW     30      // how far a window's shadow spreads
#define SHADOW_DY  12      // and how far down it is pushed

typedef struct {
    const char *name;
    int dark;                          // which wallpaper, which glass
    // The glass: the frosted wallpaper under a tint, stronger when the
    // window is not the active one -- an inactive frame goes paler and
    // quieter, the way Windows 7's did.
    uint32_t glass, glass_off;
    int      glass_a, glass_off_a;
    uint32_t rim;                      // the dark hairline round every pane
    int      rim_a, shine_a;           // ... and the light one just inside it
    int      sheen_a;                  // the gloss across the top of a frame
    uint32_t title_glow;               // halo behind title text
    int      title_glow_a;
    uint32_t accent;                   // selection, focus, progress
    uint32_t wp_a, wp_b, wp_glow;      // wallpaper gradient + glow
    uint32_t title_a, title_b, title_hl;  // title bar gradient + top highlight
    uint32_t glow, glow_dim;           // focused / unfocused window edge
    uint32_t content_bg;               // terminal background
    uint32_t bar_a, bar_b;             // taskbar gradient
    uint32_t menu_bg;                  // start menu / Alt+Tab panel tint
    // What floats over the windows -- the island, the launcher, the panel, a
    // notice -- is glass tinted with `frost` (and a little of the accent),
    // and the plates laid on that glass, a switch or a row under the pointer,
    // are `plate`. White and white in the light theme; slate in the dark.
    uint32_t frost, plate;
    int      frost_a;                  // added to each glass's own strength
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

// Not const: the accent, and everything derived from it, is the wallpaper's
// and is filled in whenever the wallpaper is (theme_wear).
static theme_t THEMES[] = {
    {   // 0: light -- the default
        .name = "Light",
        .dark = 0,
        .glass = 0x00F2F7FC, .glass_a = 100,
        .glass_off = 0x00F4F6F8, .glass_off_a = 140,
        .rim = 0x00102030, .rim_a = 120, .shine_a = 170, .sheen_a = 120,
        .title_glow = 0x00FFFFFF, .title_glow_a = 200,
        .accent = 0x003C8CE6,
        .wp_a = 0x00246FB0, .wp_b = 0x00113A63, .wp_glow = 0x0066C2E0,
        .title_a = 0x00EAF4FE, .title_b = 0x00B6D6F2, .title_hl = 0x00FFFFFF,
        .glow = 0x005AA0E0, .glow_dim = 0x008FB0D0,
        .content_bg = 0x001C2026,
        .bar_a = 0x00CFE3F7, .bar_b = 0x007FA9D6,
        .menu_bg = 0x00DCE8F5,
        .frost = 0x00FFFFFF, .plate = 0x00FFFFFF, .frost_a = 0,
        .orb = 0x003E86C8,
        .title_text = 0x001E2329, .title_text_dim = 0x006B7480,
        .bar_text = 0x00133154, .bar_dim = 0x00436486,
        .ui_win = 0x00F2F3F5, .ui_panel = 0x00FFFFFF, .ui_alt = 0x00F7F9FB,
        .ui_text = 0x001B1B1F, .ui_dim = 0x006B7280,
        .ui_line = 0x00D5D9DE, .ui_edge = 0x00B9C0C8,
        .ui_accent = 0x002A6FD6, .ui_accent2 = 0x001B4F9C,
        .ui_sel = 0x00CFE3FB, .ui_hot = 0x00E6F0FB,
        .ui_btn = 0x00ECEEF1, .ui_btndn = 0x00D3DAE2,
        .ui_bar = 0x00E9ECEF, .ui_bar2 = 0x00DDE2E7,
        .ui_warn = 0x00C24A2F, .ui_good = 0x002E7D46,
    },
    {   // 1: dark
        .name = "Dark",
        .dark = 1,
        .glass = 0x00182030, .glass_a = 130,
        .glass_off = 0x001C2028, .glass_off_a = 190,
        .rim = 0x00000000, .rim_a = 170, .shine_a = 70, .sheen_a = 50,
        .title_glow = 0x00000000, .title_glow_a = 110,
        .accent = 0x004AA0F0,
        .wp_a = 0x00121722, .wp_b = 0x00050608, .wp_glow = 0x00204A7A,
        .title_a = 0x00343A44, .title_b = 0x00161A22, .title_hl = 0x00505864,
        .glow = 0x003E9BE0, .glow_dim = 0x00303640,
        .content_bg = 0x000B0D12,
        .bar_a = 0x002A303C, .bar_b = 0x000C0E14,
        .menu_bg = 0x00101820,
        .frost = 0x00121821, .plate = 0x00313A49, .frost_a = 30,
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
    int pinned;                  // kept above every window that is not
    int alpha;                   // how opaque, 77..255 -- for a pinned window only
    char path[48];               // the program it was opened with ("" a shell)
    char arg[96];                // ... and what it was given to open

    // Tabs. Windows gathered into one frame share `grp`; `tab_seq` orders
    // their tabs; all but the one in front are `tab_hidden` -- alive, running,
    // but not drawn, not hit and not focused until their tab is picked.
    int grp, tab_seq, tab_hidden;

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
static int theme = 0;      // index into THEMES[] (0 = light glass)
// The language the desktop speaks: 0 Russian, 1 English. Every word it
// shows is written both ways where it is used, L("слово", "word"), so the
// two can never drift apart into a table nobody reads.
static int lang = 0;
#define L(ru, en) (lang ? (en) : (ru))

// --- the keyboard layout ------------------------------------------------------
//
// The keyboard drivers know the keys by their Latin letters. With the Russian
// layout on, a letter going to a program is the one printed beside it on a
// Russian keyboard (ЙЦУКЕН), sent as the two bytes of its UTF-8 -- a program
// that only appends what it is given builds correct text without knowing.
// Not with Ctrl, Alt or Win held: Ctrl+C is Ctrl+C in any layout.
static int kbd_ru;
static int kbd_chord;           // Alt and Shift went down together, nothing else yet

static const char ru_keys[]   = "qwertyuiop[]asdfghjkl;'zxcvbnm,./`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~@#$^&|";
static const uint16_t ru_cps[] = {
    0x439, 0x446, 0x443, 0x43A, 0x435, 0x43D, 0x433, 0x448, 0x449, 0x437, 0x445, 0x44A,   // й..ъ
    0x444, 0x44B, 0x432, 0x430, 0x43F, 0x440, 0x43E, 0x43B, 0x434, 0x436, 0x44D,          // ф..э
    0x44F, 0x447, 0x441, 0x43C, 0x438, 0x442, 0x44C, 0x431, 0x44E, '.', 0x451,            // я..ю . ё
    0x419, 0x426, 0x423, 0x41A, 0x415, 0x41D, 0x413, 0x428, 0x429, 0x417, 0x425, 0x42A,   // Й..Ъ
    0x424, 0x42B, 0x412, 0x410, 0x41F, 0x420, 0x41E, 0x41B, 0x414, 0x416, 0x42D,          // Ф..Э
    0x42F, 0x427, 0x421, 0x41C, 0x418, 0x422, 0x42C, 0x411, 0x42E, ',', 0x401,            // Я..Ю , Ё
    '"', 0x2116, ';', ':', '?', '/',                                                      // " № ; : ? /
};

static uint32_t ru_of(char a) {
    for (int i = 0; ru_keys[i]; i++) if (ru_keys[i] == a) return ru_cps[i];
    return 0;
}

static void kbd_toggle(void) {
    kbd_ru = !kbd_ru;
    dirty = 1;
}

int wm_kbd_ru(void) { return kbd_ru; }
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
static int  collect_windows_ex(int ws, int *out, int max, int all);
static int  topmost_window(int ws);

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

static int grp_next, tab_seq_next;     // numbers handed out, never reused

static int alloc_node(void) {
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used) {
            nodes[i].used = 1;
            nodes[i].z = 0;
            nodes[i].state = WIN_NORMAL;
            nodes[i].pinned = 0;
            nodes[i].alpha = 255;
            nodes[i].grp = ++grp_next;          // a group of its own
            nodes[i].tab_seq = ++tab_seq_next;
            nodes[i].tab_hidden = 0;
            irq_restore(f);
            return i;
        }
    }
    irq_restore(f);
    return -1;
}

static void tab_leave(int n);

static void free_node(int n) {
    if (n < 0) return;
    if (nodes[n].used) tab_leave(n);           // a tab beside it takes its place
    nodes[n].used = 0;
}

// --- tabs: windows sharing one frame ---------------------------------------------
//
// Windows can be gathered into one frame, a tab each, the way a browser keeps
// its pages. Each member stays a whole window -- its own pane, its own
// program -- and the group is only a number they share. The member in front
// IS the window as far as everything else is concerned; the others wait,
// hidden, and take its place, geometry and all, when their tab is picked.
#define TAB_MAX 8

// The members of n's group in the order their tabs stand. Returns how many.
static int tab_members(int n, int *out) {
    int k = 0;
    for (int i = 0; i < MAX_NODES && k < TAB_MAX; i++)
        if (nodes[i].used && nodes[i].grp == nodes[n].grp) out[k++] = i;
    for (int i = 1; i < k; i++) {
        int v = out[i], j = i - 1;
        while (j >= 0 && nodes[out[j]].tab_seq > nodes[v].tab_seq) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return k;
}

static int tab_count(int n) {
    int m[TAB_MAX];
    return tab_members(n, m);
}

// Where n's tab stands among its group's.
static int tab_index(int n) {
    int m[TAB_MAX], k = tab_members(n, m);
    for (int i = 0; i < k; i++) if (m[i] == n) return i;
    return 0;
}

static int tab_front_of(int grp) {
    for (int i = 0; i < MAX_NODES; i++)
        if (nodes[i].used && nodes[i].grp == grp && !nodes[i].tab_hidden) return i;
    return -1;
}

// Put `to` where `from` is, the way it is: same place, size, state, stack.
static void tab_take_place(int to, int from) {
    struct wm_node *a = &nodes[to];
    const struct wm_node *b = &nodes[from];
    a->x = b->x; a->y = b->y; a->w = b->w; a->h = b->h;
    a->sx = b->sx; a->sy = b->sy; a->sw = b->sw; a->sh = b->sh;
    a->state = b->state;
    a->z = b->z;
    a->pinned = b->pinned;
    a->alpha = b->alpha;
    a->ws = b->ws;
}

// How opaque a window is drawn: a pinned one can be seen through, so it can
// stay over what you work on without hiding it.
static int win_alpha(int n) {
    return nodes[n].pinned ? nodes[n].alpha : 255;
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
    uint32_t lr = gfx ? FRAME : FRAME + TPAD, top = gfx ? TITLE_H : TITLE_H + TPAD;
    uint32_t bot = gfx ? FRAME : FRAME + TPAD;
    *cx = nd->x + lr;
    *cy = nd->y + top;
    *cw = (nd->w > 2 * lr) ? nd->w - 2 * lr : 0;
    *ch = (nd->h > top + bot) ? nd->h - top - bot : 0;
}

// The desktop rectangle windows live in: everything above the taskbar.
static int island_edge_now(void);

static void desk_area(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    uint32_t W = fb_get_width(), H = fb_get_height();
    *x = AGAP;
    *y = AGAP;
    *w = (W > 2 * AGAP) ? W - 2 * AGAP : W;
    *h = (H > 2 * AGAP) ? H - 2 * AGAP : H;
    // Leave the island its band, on whichever edge it floats.
    switch (island_edge_now()) {
    case 1: *x += TASKBAR_H - AGAP; *w -= TASKBAR_H - AGAP; break;      // left
    case 2: *w -= TASKBAR_H - AGAP; break;                              // right
    case 3: *y += TASKBAR_H - AGAP; *h -= TASKBAR_H - AGAP; break;      // top
    default: *h -= TASKBAR_H - AGAP; break;                             // bottom
    }
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
    // A hidden tab keeps up with its frame, so its program is sized right
    // and its tab can come forward without a jump.
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used || !nodes[i].tab_hidden) continue;
        int f = tab_front_of(nodes[i].grp);
        if (f >= 0) tab_take_place(i, f);
        else nodes[i].tab_hidden = 0;           // a group with nobody in front
    }
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

// --- moving pixels, on every core ------------------------------------------------
//
// Copying and mixing blocks of pixels is most of what is left once the
// drawing is spread over the cores (ui.c): the wallpaper put back, a floating
// thing's keeper, an animation's pictures. Each is rows of their own, and
// ui_bands hands them out the same way. Strides are in pixels.
struct px_ctx { uint32_t *dst; const uint32_t *src; int ds, ss, w, a; };

static void px_copy_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct px_ctx *c = (const struct px_ctx *)cv;
    for (int j = y0; j < y1; j++) {
        uint32_t *d = c->dst + (size_t)j * c->ds;
        const uint32_t *src = c->src + (size_t)j * c->ss;
        int i = 0, w = c->w;
        // Two pixels a store where both sides allow it: the compiler will
        // not widen a 32-bit copy by itself without SSE.
        if (!(((uintptr_t)d ^ (uintptr_t)src) & 7)) {
            if (((uintptr_t)d & 7) && w) { d[0] = src[0]; i = 1; }
            uint64_t *d8 = (uint64_t *)(d + i);
            const uint64_t *s8 = (const uint64_t *)(src + i);
            int pairs = (w - i) / 2;
            for (int k = 0; k < pairs; k++) d8[k] = s8[k];
            i += pairs * 2;
        }
        for (; i < w; i++) d[i] = src[i];
    }
}

// dst = src mixed toward dst by a (256 = all dst).
static void px_mix_band(int y0, int y1, int share, void *cv) {
    (void)share;
    const struct px_ctx *c = (const struct px_ctx *)cv;
    for (int j = y0; j < y1; j++) {
        uint32_t *d = c->dst + (size_t)j * c->ds;
        const uint32_t *src = c->src + (size_t)j * c->ss;
        for (int i = 0; i < c->w; i++) d[i] = ui_mix(src[i], d[i], c->a);
    }
}

static void px_copy(uint32_t *dst, int ds, const uint32_t *src, int ss, int w, int h) {
    if (w <= 0 || h <= 0) return;
    struct px_ctx c = { dst, src, ds, ss, w, 0 };
    ui_bands(px_copy_band, &c, 0, h, w);
}

static void px_mix(uint32_t *dst, int ds, const uint32_t *src, int ss, int w, int h, int a) {
    if (w <= 0 || h <= 0) return;
    struct px_ctx c = { dst, src, ds, ss, w, a };
    ui_bands(px_mix_band, &c, 0, h, w * 3);
}

// The back buffer's pixel at (x, y), and its stride in pixels.
static uint32_t *bb_at(int x, int y) {
    return (uint32_t *)(fb_get_base() + (size_t)y * fb_get_pitch()) + x;
}
static int bb_stride(void) { return (int)(fb_get_pitch() / 4); }

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
    if (sw == dw && sh == dh && dx + dw <= fbw && dy + dh <= fbh) {
        px_copy(bb_at((int)dx, (int)dy), bb_stride(), src, (int)sw, (int)dw, (int)dh);
        return;
    }
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

// Put the wallpaper back over a rectangle of the screen (clipped to it).
static void restore_wall(int x, int y, int w, int h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (!wallpaper || (uint32_t)W != wp_w || (uint32_t)H != wp_h) {
        fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, COLOR_DESKTOP);
        return;
    }
    px_copy(bb_at(x, y), bb_stride(), wallpaper + (size_t)y * wp_w + x, (int)wp_w, w, h);
    fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
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
    if (p->sel_on) {
        uint32_t sel = ui_mix(TH->content_bg, TH->accent, 110);
        for (uint32_t r = p->sel_r0; r <= p->sel_r1 && r < p->rows; r++) {
            uint32_t c0 = r == p->sel_r0 ? p->sel_c0 : 0;
            uint32_t c1 = r == p->sel_r1 ? p->sel_c1 : p->cols;
            if (c1 > c0) fb_fill_rect(cx + c0 * GW, cy + r * GH, (c1 - c0) * GW, GH, sel);
        }
    }
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
    if (p->sel_on)
        hsh = (hsh ^ ((uint64_t)p->sel_r0 << 48 | (uint64_t)p->sel_c0 << 32 |
                      (uint64_t)p->sel_r1 << 16 | p->sel_c1)) * FNV_P;
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
    uint32_t look;             // caption buttons: which is lit, how far faded
    int      gfx;              // a pixel surface rather than a character grid
    uint32_t gen;              // ... and which of its frames was painted
};
static struct pane_cache pcache[MAX_PANES];

// A graphics pane's pixels are not fingerprinted -- hashing a megabyte to
// find out whether it changed costs about what painting it does. Each frame
// a program hands over counts instead.
static uint32_t gfx_gen[MAX_PANES];

static void invalidate_pane_cache(void) {
    for (int i = 0; i < MAX_PANES; i++) pcache[i].valid = 0;
}

#define GRAB   5                    // how close to an edge counts as a resize

// --- what the pointer is over ----------------------------------------------
//
// Anything that can be pressed lights up under the pointer -- a window's
// button, something on the island, a menu row -- and the light fades in and
// out over a few frames rather than switching. There is one pointer, so one
// thing is lit and one is fading out: each named by a key.
#define FADE_MS 150
#define KEY_CAP(n, b) (0x10000 | ((n) << 4) | (b))
#define KEY_TB(i)     (0x20000 | (i))
#define KEY_TAB(n, i, part) (0x50000 | ((n) << 6) | ((i) << 2) | (part))   // part 1: its close mark
static int hov_key = -1, hov_old = -1;
static uint64_t hov_ms, hov_old_ms;
static int press_key = -1;                  // what a press armed, until release

// Milliseconds, as finely as the processor's own counter allows. The timer
// ticks every 10 ms, and an animation stepped by it moves in visible jumps;
// the time-stamp counter is learned against the timer over the first half
// second, and from then on fills in the time between ticks. It is kept within
// a tick of the timer, so the two can never drift apart.
static uint64_t clk_tick0, clk_tsc0, clk_per_ms;
static uint64_t now_ms(void) {
    uint64_t t = pit_get_ticks(), c = rdtsc();
    if (!clk_tsc0) { clk_tick0 = t; clk_tsc0 = c; return t * 10; }
    if (!clk_per_ms) {
        if (t - clk_tick0 < 50) return t * 10;
        clk_per_ms = (c - clk_tsc0) / ((t - clk_tick0) * 10);
        if (!clk_per_ms) clk_per_ms = 1;
    }
    uint64_t f = clk_tick0 * 10 + (c - clk_tsc0) / clk_per_ms;
    if (f < t * 10) f = t * 10;
    if (f > t * 10 + 10) f = t * 10 + 10;
    return f;
}
static uint64_t last_frame_ms;         // when render_all last ran

// --- a spring ---------------------------------------------------------------------
//
// What arrives -- a window opening, a menu coming up, a notice sliding in --
// moves like a weight on a spring: quickly, a little past where it stops, and
// back. The curve is a damped spring's (damping 0.65, 22 rad/s), worked out
// once in 1/256ths at 33 even steps over SPRING_MS; between steps, a line.
// What leaves does not bounce: it just goes.
#define SPRING_MS 380
static const int16_t spring_tab[33] = {
    0, 8, 28, 55, 86, 118, 148, 176, 201, 222, 238, 251, 261, 267, 271, 273, 273,
    273, 271, 269, 267, 265, 263, 261, 259, 258, 257, 256, 256, 255, 255, 255, 256,
};

// Where the spring is `el` ms in: 0 at rest before, 256 at rest after.
static int spring_at(uint64_t el) {
    if (el >= SPRING_MS) return 256;
    int pos = (int)(el * 32 * 256 / SPRING_MS);       // the step, in 1/256ths
    int i = pos >> 8, f = pos & 255;
    return spring_tab[i] + (((spring_tab[i + 1] - spring_tab[i]) * f) >> 8);
}

static int fade_in(uint64_t since) {
    uint64_t t = now_ms() - since;
    return t >= FADE_MS ? 256 : (int)(t * 256 / FADE_MS);
}

// How lit `key` is, 0..256.
static int glow_of(int key) {
    if (key < 0) return 0;
    if (key == hov_key) return fade_in(hov_ms);
    if (key == hov_old) return 256 - fade_in(hov_old_ms);
    return 0;
}

// Something is still fading: the compositor owes another frame.
static int hover_animating(void) {
    return (hov_key >= 0 && fade_in(hov_ms) < 256) ||
           (hov_old >= 0 && fade_in(hov_old_ms) < 256);
}

// Returns 1 if that changed anything.
static int hover_to(int key) {
    if (key == hov_key) return 0;
    if (hov_key >= 0) { hov_old = hov_key; hov_old_ms = now_ms(); }
    hov_key = key;
    hov_ms = now_ms();
    return 1;
}

// --- a window's top: its tab, and its buttons --------------------------------
//
// The top of a window is a strip of coloured glass holding its tab -- a
// shape in the colour of what is inside the window, flowing down into it --
// and, at the right, four round buttons: pin it above everything, minimise,
// maximise, close. Drawing and hit-testing both ask the functions here where
// things are, so the two cannot disagree.

#define WB_PIN   0
#define WB_MIN   1
#define WB_MAX   2
#define WB_CLOSE 3
#define WB_N     4
#define WB_TAB   4                 // the close mark on the tab, as a fifth button
#define WB_PLUS  5                 // the "+" after the tabs: a new terminal tab
#define WBTN     28
#define WBTN_GAP 6

static void cap_rect(const struct wm_node *nd, int which, int *bx, int *by, int *bw, int *bh) {
    int right = (int)(nd->x + nd->w) - 10;
    *bw = WBTN;
    *bh = WBTN;
    *by = (int)nd->y + (TITLE_H - WBTN) / 2 + 1;
    *bx = right - (WB_N - which) * (WBTN + WBTN_GAP) + WBTN_GAP;
}

static int cap_glow(int n, int b) { return glow_of(KEY_CAP(n, b)); }

static int join_hint = -1;          // the window a dragged tab would join

static const char *pane_title(struct pane *p);
static const char *program_label(const char *file);

// Everything about a frame's top that can change without the window moving:
// what is lit, what is held, the other tabs and their names.
static uint32_t win_look(int n) {
    uint32_t v = 0;
    for (int b = 0; b <= WB_PLUS; b++) v = v * 131 + (uint32_t)cap_glow(n, b);
    for (int b = 0; b <= WB_PLUS; b++)
        if (press_key == KEY_CAP(n, b)) v ^= 0x80000000u | (uint32_t)b << 27;
    int m[TAB_MAX], k = tab_members(n, m);
    for (int i = 0; i < k; i++) {
        for (int part = 0; part < 2; part++) {
            v = v * 131 + (uint32_t)glow_of(KEY_TAB(n, i, part));
            if (press_key == KEY_TAB(n, i, part)) v ^= 0x40000000u | (uint32_t)(i * 2 + part) << 20;
        }
        for (const char *t = pane_title(&panes[nodes[m[i]].pane_idx]); *t; t++)
            v = v * 33 + (uint8_t)*t;
        v = v * 7 + (uint32_t)m[i];
    }
    if (join_hint == n) v ^= 0x20000000u;
    v = v * 257 + (uint32_t)win_alpha(n);
    return v;
}

// The rectangle inside the frame that the window's program owns.
static void client_rect(int n, int *x, int *y, int *w, int *h) {
    const struct wm_node *nd = &nodes[n];
    *x = (int)nd->x + FRAME;
    *y = (int)nd->y + TITLE_H;
    *w = (int)nd->w - 2 * FRAME;
    *h = (int)nd->h - TITLE_H - FRAME;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

// The colour of what the window shows, which its tab takes so the two read
// as one piece: the terminal's background, or the top row of a program's
// own picture.
static uint32_t body_colour(int n) {
    struct pane *p = &panes[nodes[n].pane_idx];
    if (!p->gfx_on || !p->gfx || !p->gfx_w) return TH->content_bg;
    uint32_t r = 0, g = 0, b = 0;
    int k = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t c = p->gfx[(uint32_t)i * p->gfx_w / 16];
        r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
        k++;
    }
    return ((r / k) << 16) | ((g / k) << 8) | (b / k);
}

static int is_light(uint32_t c) {
    int y = (int)(((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8;
    return y > 150;
}

// Where the tabs are. They stand in a row from the left of the strip, each
// as wide as its name wants, all narrower when they would not fit; then the
// "+", then room up to the buttons. n is the window in front.
#define TAB_GAP 6
#define PLUS_S  28

static void tab_rect_i(int n, int i, int *tx, int *ty, int *tw, int *th) {
    const struct wm_node *nd = &nodes[n];
    int m[TAB_MAX], k = tab_members(n, m);
    int bx, by, bw, bh;
    cap_rect(nd, 0, &bx, &by, &bw, &bh);
    int left = (int)nd->x + FRAME;
    int room = bx - 12 - PLUS_S - TAB_GAP - left;
    int w[TAB_MAX], total = 0;
    for (int j = 0; j < k; j++) {
        int ww = 38 + ui_text_w(program_label(pane_title(&panes[nodes[m[j]].pane_idx])), UI_F13B) + 40;
        if (ww > 240) ww = 240;
        if (ww < 60) ww = 60;
        w[j] = ww;
        total += ww + (j ? TAB_GAP : 0);
    }
    if (total > room && k) {
        int each = (room - (k - 1) * TAB_GAP) / k;
        if (each < 44) each = 44;
        for (int j = 0; j < k; j++) if (w[j] > each) w[j] = each;
    }
    int x = left;
    for (int j = 0; j < i && j < k; j++) x += w[j] + TAB_GAP;
    *tx = x;
    *ty = (int)nd->y + 7;
    *tw = i < k ? w[i] : 0;
    *th = TITLE_H - 7;
}

// The tab of the window itself.
static void tab_rect(int n, int *tx, int *ty, int *tw, int *th) {
    tab_rect_i(n, tab_index(n), tx, ty, tw, th);
}

static void tab_close_rect_i(int n, int i, int *x, int *y, int *w, int *h) {
    int tx, ty, tw, th;
    tab_rect_i(n, i, &tx, &ty, &tw, &th);
    *w = 22; *h = 22;
    *x = tx + tw - 30;
    *y = ty + (th - 22) / 2;
}

static void tab_close_rect(int n, int *x, int *y, int *w, int *h) {
    tab_close_rect_i(n, tab_index(n), x, y, w, h);
}

// A tab behind shows its close mark only when it is wide enough to spare it.
static int tab_has_close(int n, int i) {
    if (i == tab_index(n)) return 1;
    int tx, ty, tw, th;
    tab_rect_i(n, i, &tx, &ty, &tw, &th);
    return tw >= 90;
}

static void plus_rect(int n, int *x, int *y, int *w, int *h) {
    int k = tab_count(n);
    int tx, ty, tw, th;
    tab_rect_i(n, k - 1, &tx, &ty, &tw, &th);
    *x = tx + tw + TAB_GAP;
    *y = (int)nodes[n].y + (TITLE_H - PLUS_S) / 2 + 1;
    *w = PLUS_S;
    *h = PLUS_S;
}

// What of the row of tabs is at (mx, my), and which tab.
#define TABH_NONE  0
#define TABH_TAB   1
#define TABH_CLOSE 2
#define TABH_PLUS  3

static int tab_hit(int n, int mx, int my, int *idx) {
    const struct wm_node *nd = &nodes[n];
    *idx = -1;
    if (my < (int)nd->y + 5 || my >= (int)nd->y + TITLE_H) return TABH_NONE;
    int px, py, pw, ph;
    plus_rect(n, &px, &py, &pw, &ph);
    if (mx >= px && mx < px + pw && my >= py - 2 && my < py + ph + 2) return TABH_PLUS;
    int k = tab_count(n);
    for (int i = 0; i < k; i++) {
        int tx, ty, tw, th;
        tab_rect_i(n, i, &tx, &ty, &tw, &th);
        if (mx < tx || mx >= tx + tw) continue;
        *idx = i;
        if (tab_has_close(n, i)) {
            int cx, cy, cw, ch;
            tab_close_rect_i(n, i, &cx, &cy, &cw, &ch);
            if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) return TABH_CLOSE;
        }
        return TABH_TAB;
    }
    return TABH_NONE;
}

// --- soft glyphs: the window's buttons, and the island's ----------------------

#define FXI(v) RAST_INT(v)

static void g_fill(rast_path *p, uint32_t argb) {
    rast_paint pt;
    rast_paint_solid(&pt, argb);
    ui_rast_fill(p, &pt);
}

static void g_stroke(rast_path *p, rast_fx wdt, uint32_t argb) {
    rast_stroke s;
    rast_stroke_init(&s, wdt);
    s.cap = RAST_CAP_ROUND;
    s.join = RAST_JOIN_ROUND;
    rast_paint pt;
    rast_paint_solid(&pt, argb);
    ui_rast_stroke(p, &s, &pt);
}

#define GL_PIN   0
#define GL_MIN   1
#define GL_MAX   2
#define GL_CLOSE 3
#define GL_RESTORE 4
#define GL_PLUS  5

// A glyph in a box of size s at (x, y).
static void wglyph(int g, int x, int y, int s, uint32_t c) {
    rast_path p;
    rast_path_init(&p);
    rast_fx X = FXI(x), Y = FXI(y), S = FXI(s);
#define PT(a, b) X + (rast_fx)((int64_t)S * (a) / 100), Y + (rast_fx)((int64_t)S * (b) / 100)
#define LEN(a) (rast_fx)((int64_t)S * (a) / 100)
    switch (g) {
    case GL_PIN:
        rast_round_rect(&p, PT(30, 8), LEN(40), LEN(40), LEN(10), LEN(10));
        rast_move_to(&p, PT(16, 52)); rast_line_to(&p, PT(84, 52));
        rast_line_to(&p, PT(70, 38)); rast_line_to(&p, PT(30, 38)); rast_close(&p);
        g_fill(&p, c);
        rast_path_reset(&p);
        rast_move_to(&p, PT(50, 52)); rast_line_to(&p, PT(50, 92));
        g_stroke(&p, LEN(10), c);
        break;
    case GL_MIN:
        rast_move_to(&p, PT(22, 52)); rast_line_to(&p, PT(78, 52));
        g_stroke(&p, LEN(11), c);
        break;
    case GL_MAX:
        rast_round_rect(&p, PT(22, 22), LEN(56), LEN(56), LEN(16), LEN(16));
        g_stroke(&p, LEN(10), c);
        break;
    case GL_PLUS:
        rast_move_to(&p, PT(50, 20)); rast_line_to(&p, PT(50, 80));
        rast_move_to(&p, PT(20, 50)); rast_line_to(&p, PT(80, 50));
        g_stroke(&p, LEN(11), c);
        break;
    case GL_RESTORE:
        rast_round_rect(&p, PT(34, 16), LEN(48), LEN(48), LEN(14), LEN(14));
        g_stroke(&p, LEN(9), c);
        rast_path_reset(&p);
        rast_round_rect(&p, PT(18, 36), LEN(48), LEN(48), LEN(14), LEN(14));
        g_fill(&p, c);
        break;
    default:
        rast_move_to(&p, PT(26, 26)); rast_line_to(&p, PT(74, 74));
        rast_move_to(&p, PT(74, 26)); rast_line_to(&p, PT(26, 74));
        g_stroke(&p, LEN(11), c);
        break;
    }
#undef PT
#undef LEN
    rast_path_free(&p);
}

// --- the window frame ----------------------------------------------------------

// Each window's glass, with its memory of what lies under it.
static ui_glass wglass[MAX_NODES];
uint64_t pf_sh, pf_tk, pf_gl, pf_ch;     // a window's parts: shadow, glass memory, glass, frame

// Bring a hidden tab to the front of its frame, in the place of the one there.
static void tab_show(int n) {
    if (!nodes[n].used || !nodes[n].tab_hidden) return;
    int f = tab_front_of(nodes[n].grp);
    if (f >= 0) {
        tab_take_place(n, f);
        nodes[f].tab_hidden = 1;
        if (focused == f) focused = n;
    }
    nodes[n].tab_hidden = 0;
    ui_glass_forget(&wglass[n]);           // its memory of the desktop is old
    wp_dirty = 1;
    dirty = 1;
}

// Take n out of its group into one of its own. If it was in front, the tab
// beside it comes forward in its place, so the frame stays where it was.
static void tab_leave(int n) {
    int m[TAB_MAX], k = tab_members(n, m);
    if (k > 1 && !nodes[n].tab_hidden) {
        int at = 0;
        for (int i = 0; i < k; i++) if (m[i] == n) at = i;
        int nb = at + 1 < k ? m[at + 1] : m[at - 1];
        tab_take_place(nb, n);
        nodes[nb].tab_hidden = 0;
        ui_glass_forget(&wglass[nb]);
        if (focused == n) focused = nb;
    }
    nodes[n].grp = ++grp_next;
    nodes[n].tab_hidden = 0;
    wp_dirty = 1;
}

// Make n the newest tab of target's frame, in front. 0 if the frame is full.
static int tab_join(int n, int target) {
    if (nodes[n].grp == nodes[target].grp) return 1;
    int f = tab_front_of(nodes[target].grp);
    if (f < 0 || tab_count(f) >= TAB_MAX) return 0;
    tab_leave(n);
    nodes[n].grp = nodes[f].grp;
    nodes[n].tab_seq = ++tab_seq_next;
    tab_take_place(n, f);
    nodes[f].tab_hidden = 1;
    nodes[n].tab_hidden = 0;
    ui_glass_forget(&wglass[n]);
    focused = n;
    wp_dirty = 1;
    dirty = 1;
    return 1;
}

// A whole frame of tabs let go on another: all of them move over, in their
// order, and the one that was in front stays in front.
static void group_join(int n, int target) {
    int m[TAB_MAX], k = tab_members(n, m);
    if (tab_count(target) + k > TAB_MAX) return;
    for (int i = 0; i < k; i++) if (m[i] != n) tab_join(m[i], target);
    tab_join(n, target);
}

// Put n's tab at place `to` in its row.
static void tab_move_to(int n, int to) {
    int m[TAB_MAX], k = tab_members(n, m), o[TAB_MAX], q = 0;
    for (int i = 0; i < k; i++) if (m[i] != n) o[q++] = m[i];
    if (to > q) to = q;
    if (to < 0) to = 0;
    for (int i = q; i > to; i--) o[i] = o[i - 1];
    o[to] = n;
    for (int i = 0; i <= q; i++) nodes[o[i]].tab_seq = ++tab_seq_next;
    wp_dirty = 1;
    dirty = 1;
}

// Where the window and its shadow reach: x0, y0, x1, y1.
static void window_extent(int n, int e[4]) {
    const struct wm_node *nd = &nodes[n];
    int x = (int)nd->x, y = (int)nd->y, w = (int)nd->w, h = (int)nd->h;
    if (nd->state == WIN_MAX) {
        e[0] = x; e[1] = y; e[2] = x + w; e[3] = y + h;
        return;
    }
    e[0] = x - SHADOW;
    e[1] = y - SHADOW + SHADOW_DY;
    e[2] = x + w + SHADOW;
    e[3] = y + h + SHADOW + SHADOW_DY;
}

static int boxes_meet(const int a[4], const int b[4]) {
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

// The body's rounded corners. The top left is square when the tab in front
// is the first one, standing right over it and flowing down into it.
static int body_corners(int n) {
    return tab_index(n) == 0 ? (UI_TR | UI_BL | UI_BR) : UI_ALL;
}

// The glass of a window: a frame round its body, the strip at the top.
static void window_glass(int n, int focus) {
    const struct wm_node *nd = &nodes[n];
    int x = (int)nd->x, y = (int)nd->y, w = (int)nd->w, h = (int)nd->h;
    int maxed = nd->state == WIN_MAX;
    int R = maxed ? 0 : RAD_T;
    int bx, by, bw, bh;
    client_rect(n, &bx, &by, &bw, &bh);
    ui_glass_draw(&wglass[n], x, y, w, h, R, bx, by, bw, bh, BODY_R, body_corners(n),
                  focus ? TH->glass : TH->glass_off, focus ? TH->glass_a : TH->glass_off_a);
}

static int u2s(char *b, unsigned v);

static void paint_chrome(int n, int focus) {
    const struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    const theme_t *T = TH;
    int x = (int)nd->x, y = (int)nd->y, w = (int)nd->w, h = (int)nd->h;
    int maxed = nd->state == WIN_MAX;
    int R = maxed ? 0 : RAD_T;

    uint64_t g0 = rdtsc();
    window_glass(n, focus);
    uint64_t g1 = rdtsc();
    pf_gl += g1 - g0;

    // Light on the glass: a sheen down the strip, a fine bright edge round
    // the whole window and a faint dark one just outside it, so a pale window
    // still has an outline against a pale wallpaper.
    int sa = T->dark ? (focus ? 24 : 14) : (focus ? 60 : 36);
    ui_mat sheen = { UI_GRAD, 0x00FFFFFF, 0x00FFFFFF, sa, 0, y, y + TITLE_H };
    ui_rrect(x, y, w, TITLE_H, R, UI_TOP, &sheen);
    if (!maxed) {
        ui_rrect_line(x, y, w, h, R, UI_ALL, 0x00FFFFFF, T->dark ? (focus ? 70 : 50) : (focus ? 150 : 110));
        ui_rrect_line(x + 1, y + 1, w - 2, h - 2, R - 1, UI_ALL, 0x00FFFFFF, 30);
    }

    // The tabs behind: soft plates on the glass, a name on each.
    int fi = tab_index(n);
    {
        int m[TAB_MAX], k = tab_members(n, m);
        int saved[4];
        for (int i = 0; i < k; i++) {
            if (i == fi) continue;
            int qx, qy, qw, qh;
            tab_rect_i(n, i, &qx, &qy, &qw, &qh);
            int glow = glow_of(KEY_TAB(n, i, 0));
            ui_round_fill(qx, qy + 3, qw, qh - 9, 12, T->plate,
                          (focus ? 120 : 80) + glow * 90 / 256);
            const char *nm = program_label(pane_title(&panes[nodes[m[i]].pane_idx]));
            int ix = qx + 10, iy = qy + 3 + (qh - 9 - 16) / 2;
            if (!ui_icon(ix, iy, nm, ICON_SMALL))
                ui_round_fill(ix + 2, iy + 2, 12, 12, 6, T->title_text_dim, 200);
            int cl = tab_has_close(n, i);
            ui_clip_get(saved);
            ui_clip(qx, qy, qw - (cl ? 32 : 8), qh);
            ui_text_fit(ix + 24, qy + 3 + (qh - 9 - ui_line_h(UI_F13)) / 2, qw - 34 - (cl ? 30 : 4), nm,
                        UI_F13, glow > 128 ? T->title_text : T->title_text_dim);
            ui_clip_set(saved);
            if (cl) {
                int cx, cy, cw, chh;
                tab_close_rect_i(n, i, &cx, &cy, &cw, &chh);
                int cg = glow_of(KEY_TAB(n, i, 1));
                if (cg) ui_round_fill(cx, cy, cw, chh, 11, T->dark ? 0x00FFFFFF : 0x00000000, cg * 30 / 256);
                if (glow || cg) wglyph(GL_CLOSE, cx + 5, cy + 5, 12, 0xFF000000 | T->title_text_dim);
            }
        }
        // The "+": another terminal, as a tab of this window. Where a dragged
        // tab would land, a plate in the accent shows the place.
        int px, py, pw, ph;
        plus_rect(n, &px, &py, &pw, &ph);
        if (join_hint == n) {
            ui_round_fill(px, py, 120, ph, 12, T->accent, 120);
        } else {
            int pg = glow_of(KEY_CAP(n, WB_PLUS));
            ui_round_fill(px, py, pw, ph, ph / 2, T->plate, (focus ? 80 : 50) + pg * 140 / 256);
            wglyph(GL_PLUS, px + 8, py + 8, 12, 0xFF000000 | (pg > 128 ? T->title_text : T->title_text_dim));
        }
    }

    // The tab in front: in the body's colour, rounded at the top, its feet
    // curving out into the strip so the tab and the body are one shape.
    uint32_t bc = body_colour(n);
    int tx, ty, tw, th;
    tab_rect(n, &tx, &ty, &tw, &th);
    ui_mat tabm = { UI_SOLID, bc, 0, 255, 0, 0, 0 };
    ui_rrect(tx, ty, tw, th + 2, 12, UI_TOP, &tabm);
    {
        rast_path fp;
        rast_path_init(&fp);
        int fy = ty + th;                        // the strip's bottom = body top
        rast_move_to(&fp, FXI(tx + tw + 10), FXI(fy));
        rast_quad_to(&fp, FXI(tx + tw), FXI(fy), FXI(tx + tw), FXI(fy - 10));
        rast_line_to(&fp, FXI(tx + tw), FXI(fy));
        rast_close(&fp);
        if (fi > 0) {                            // and on the left, not first
            rast_move_to(&fp, FXI(tx - 10), FXI(fy));
            rast_quad_to(&fp, FXI(tx), FXI(fy), FXI(tx), FXI(fy - 10));
            rast_line_to(&fp, FXI(tx), FXI(fy));
            rast_close(&fp);
        }
        g_fill(&fp, 0xFF000000 | bc);
        rast_path_free(&fp);
    }
    uint32_t ink = is_light(bc) ? T->title_text : 0x00F2F4F7;
    uint32_t dim = is_light(bc) ? T->title_text_dim : 0x009AA3AE;
    {
        const char *ttl = pane_title(p);
        int ix = tx + 12, iy = ty + (th - 18) / 2;
        if (!ui_icon(ix + 1, iy + 1, ttl, ICON_SMALL)) {
            // No drawn icon yet: a dot of the system's colour.
            ui_round_fill(ix + 2, iy + 2, 14, 14, 7, T->accent, 255);
        }
        int saved[4];
        ui_clip_get(saved);
        ui_clip(tx, ty, tw - 36, th);
        ui_text_fit(ix + 26, ty + (th - ui_line_h(UI_F13B)) / 2, tw - 36 - 38, program_label(ttl), UI_F13B,
                    focus ? ink : dim);
        ui_clip_set(saved);
        int cx, cy, cw, chh;
        tab_close_rect(n, &cx, &cy, &cw, &chh);
        int glow = glow_of(KEY_TAB(n, fi, 1));
        if (glow) ui_round_fill(cx, cy, cw, chh, 11, is_light(bc) ? 0x00000000 : 0x00FFFFFF, glow * 30 / 256);
        wglyph(GL_CLOSE, cx + 5, cy + 5, 12, 0xFF000000 | dim);
    }

    // On the pin of a pinned window: the wheel sets how much shows through.
    if (nd->pinned && cap_glow(n, WB_PIN)) {
        char t[48];
        int q = 0;
        const char *a = L("Колесо мыши — прозрачность · ", "Mouse wheel — opacity · ");
        while (*a) t[q++] = *a++;
        q += u2s(t + q, (unsigned)(nd->alpha * 100 + 127) / 255);
        t[q++] = '%';
        t[q] = 0;
        int bx, by, bw, bh;
        cap_rect(nd, WB_PIN, &bx, &by, &bw, &bh);
        int tw = ui_text_w(t, UI_F12);
        ui_text(bx - 12 - tw, y + (TITLE_H - ui_line_h(UI_F12)) / 2 + 1, t, UI_F12,
                is_light(T->glass) ? T->title_text : 0x00F2F4F7);
    }

    // The buttons.
    static const int glyphs[WB_N] = { GL_PIN, GL_MIN, GL_MAX, GL_CLOSE };
    for (int b = 0; b < WB_N; b++) {
        int bx, by, bw, bh;
        cap_rect(nd, b, &bx, &by, &bw, &bh);
        int glow = cap_glow(n, b);
        int pressed = press_key == KEY_CAP(n, b) && glow;
        uint32_t fill = T->plate, gc = 0xFF000000 | T->title_text;
        int a = focus ? 110 : 70;
        a += glow * (focus ? 120 : 140) / 256;
        if (b == WB_CLOSE && glow) {
            fill = ui_mix(T->plate, 0x00E5484D, glow);
            a = 120 + glow * 135 / 256;
            gc = 0xFF000000 | ui_mix(T->title_text, 0x00FFFFFF, glow);
        }
        if (b == WB_PIN && nd->pinned) { fill = T->accent; a = 255; gc = 0xFFFFFFFF; }
        if (pressed) a = 255, fill = ui_mix(fill, 0, 30);
        ui_round_fill(bx, by, bw, bh, bw / 2, fill, a);
        int g = glyphs[b];
        if (b == WB_MAX && maxed) g = GL_RESTORE;
        wglyph(g, bx + 7, by + 7, 14, gc);
    }
}

// Paint the corners of a window's body again -- the frame's glass over the
// program's picture, at the rounded edge -- after the picture alone was
// redrawn (present_pane_only) and squared them off.
static void repair_body_corners(int n) {
    if (!wglass[n].have) return;
    int bx, by, bw, bh;
    client_rect(n, &bx, &by, &bw, &bh);
    int saved[4];
    ui_clip_get(saved);
    int sq[4][2] = { { bx + bw - BODY_R, by }, { bx, by + bh - BODY_R },
                     { bx + bw - BODY_R, by + bh - BODY_R }, { bx, by } };
    int nsq = (body_corners(n) & UI_TL) ? 4 : 3;
    for (int k = 0; k < nsq; k++) {
        ui_clip(sq[k][0], sq[k][1], BODY_R, BODY_R);
        window_glass(n, n == focused);
    }
    ui_clip_set(saved);
}

// --- this frame's damage --------------------------------------------------------
//
// Which parts of the screen were painted this frame, so each pane of glass
// can refresh its memory of what lies under it there -- and only there.

#define DMG_MAX 96
static int dmg[DMG_MAX][4], ndmg, dmg_all;

static void dmg_add(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (ndmg >= DMG_MAX) { dmg_all = 1; return; }
    dmg[ndmg][0] = x0; dmg[ndmg][1] = y0; dmg[ndmg][2] = x1; dmg[ndmg][3] = y1;
    ndmg++;
}

// Bring a pane of glass's memory up to date with the back buffer as it is
// now -- which must be, where the damage is, what lies under the glass.
static void glass_catch_up(ui_glass *g) {
    if (dmg_all) ui_glass_forget(g);
    ui_glass_take(g, (const int (*)[4])dmg, ndmg);
}

// Graphics-mode panes and terminals alike: the content inside the client area.
uint64_t pf_fp, pf_paint, pf_content, pf_blit;
int      pf_painted, pf_skipped, pf_blits;

// Whether the pane looks different from what was last painted for it.
static int pane_changed(int n) {
    struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    struct pane_cache *pc = &pcache[nd->pane_idx];
    if (!pc->valid || pc->gfx != p->gfx_on) return 1;
    if (p->gfx_on) {
        if (pc->gen != gfx_gen[nd->pane_idx]) return 1;
    } else {
        uint64_t f0 = rdtsc();
        uint64_t fp = pane_fingerprint(p);
        pf_fp += rdtsc() - f0;
        if (pc->fp != fp) return 1;
    }
    return pc->x != nd->x || pc->y != nd->y || pc->w != nd->w ||
           pc->h != nd->h || pc->focus != (n == focused) || pc->theme != theme ||
           pc->look != win_look(n);
}

// What changed about a window since it was painted: nothing (0), only its
// top -- a button lit, a tab's name -- (1), only what its program shows (2),
// or more than that (3).
#define CH_NONE    0
#define CH_CHROME  1
#define CH_CONTENT 2
#define CH_ALL     3
static int win_alpha(int n);

static int change_kind(int n) {
    struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    struct pane_cache *pc = &pcache[nd->pane_idx];
    if (!pc->valid || pc->gfx != p->gfx_on) return CH_ALL;
    if (pc->x != nd->x || pc->y != nd->y || pc->w != nd->w || pc->h != nd->h ||
        pc->focus != (n == focused) || pc->theme != theme) return CH_ALL;
    if (win_alpha(n) < 255) return pane_changed(n) ? CH_ALL : CH_NONE;
    int content = p->gfx_on ? pc->gen != gfx_gen[nd->pane_idx] : pc->fp != pane_fingerprint(p);
    int look = pc->look != win_look(n);
    if (content && look) return CH_ALL;
    return look ? CH_CHROME : content ? CH_CONTENT : CH_NONE;
}

static void busy_end(int pane);

static void note_push(const char *title, const char *body, int glyph, uint32_t color);
static void note_open_last(const char *path);
static void str_put(char *dst, const char *src, int cap);

// What a USB device that came or went is, in a word, and the line under it.
static void usb_note(const struct usb_news *n) {
    static char body[96];
    const char *title;
    int in = n->attached;
    switch (n->kind) {
    case USB_KIND_KBD:
        title = in ? L("Клавиатура подключена", "Keyboard connected")
                   : L("Клавиатура отключена", "Keyboard removed");
        break;
    case USB_KIND_MOUSE:
        title = in ? L("Мышь подключена", "Mouse connected") : L("Мышь отключена", "Mouse removed");
        break;
    case USB_KIND_COMBO:
        title = in ? L("Приёмник клавиатуры и мыши подключён", "Keyboard and mouse receiver connected")
                   : L("Приёмник клавиатуры и мыши отключён", "Keyboard and mouse receiver removed");
        break;
    case USB_KIND_DISK:
        title = in ? L("Флешка подключена", "USB drive connected") : L("Флешка отключена", "USB drive removed");
        break;
    case USB_KIND_NET:
        title = in ? L("Интернет через телефон", "Internet through the phone")
                   : L("Телефон отключён", "Phone disconnected");
        break;
    case USB_KIND_HUB:
        title = in ? L("USB-разветвитель подключён", "USB hub connected")
                   : L("USB-разветвитель отключён", "USB hub removed");
        break;
    default:
        title = in ? L("USB-устройство подключено", "USB device connected")
                   : L("USB-устройство отключено", "USB device removed");
        break;
    }
    // The line under it: its own name, a drive's size, or what is missing.
    int k = 0;
    const char *name = n->name[0] ? n->name : "";
    for (int i = 0; name[i] && k < 60; i++) body[k++] = name[i];
    if (in && n->kind == USB_KIND_DISK && n->mib) {
        char num[16];
        uint32_t v = n->mib >= 1024 ? (n->mib + 512) / 1024 : n->mib;
        int m = 0;
        do { num[m++] = (char)('0' + v % 10); v /= 10; } while (v);
        if (k) { body[k++] = ','; body[k++] = ' '; }
        while (m) body[k++] = num[--m];
        const char *unit = n->mib >= 1024 ? L(" ГБ", " GB") : L(" МБ", " MB");
        for (int i = 0; unit[i]; i++) body[k++] = unit[i];
    }
    if (in && n->kind == USB_KIND_OTHER) {
        const char *t = L("Для него в системе нет драйвера.", "The system has no driver for it.");
        if (k) body[k++] = ' ', body[k++] = '-', body[k++] = ' ';
        for (int i = 0; t[i] && k < 94; i++) body[k++] = t[i];
    }
    body[k] = 0;
    // A drive: what its volume is called, and a click to open it.
    char open[16] = "";
    if (in && n->kind == USB_KIND_DISK && n->disk >= 0) {
        static struct vol_info vi[VOL_MAX];
        int nv = vol_list(vi, VOL_MAX);
        for (int i = 0; i < nv; i++) {
            if (vi[i].disk != n->disk) continue;
            open[0] = '/';
            str_put(open + 1, vi[i].mount, sizeof open - 1);
            k = 0;
            const char *lab = vi[i].label[0] ? vi[i].label : name;
            for (int j = 0; lab[j] && k < 40; j++) body[k++] = lab[j];
            const char *t = L(" — нажмите, чтобы открыть", " -- click to open");
            for (int j = 0; t[j] && k < 94; j++) body[k++] = t[j];
            body[k] = 0;
            break;
        }
    }
    sound_play(in ? SND_USB_IN : SND_USB_OUT);
    note_push(title, body, G_CHIP, in ? TH->accent : 0x006B7480);
    if (open[0]) note_open_last(open);
}

static void pane_painted(int n) {
    struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    struct pane_cache *pc = &pcache[nd->pane_idx];
    // A text program is up once it has printed something.
    if (!p->gfx_on && (p->cursor_row || p->cursor_col)) busy_end(nd->pane_idx);
    pc->valid = 1;
    pc->gfx = p->gfx_on;
    pc->gen = gfx_gen[nd->pane_idx];
    pc->fp = p->gfx_on ? 0 : pane_fingerprint(p);
    pc->x = nd->x; pc->y = nd->y; pc->w = nd->w; pc->h = nd->h;
    pc->focus = (n == focused);
    pc->theme = theme;
    pc->look = win_look(n);
}

// What lies under a window that is seen through, while it is painted.
static uint32_t *see_buf;
static int see_cap;

static void see_take(int x, int y, int w, int h) {
    if (w * h > see_cap) {
        if (see_buf) kfree(see_buf);
        see_buf = (uint32_t *)kmalloc((size_t)w * h * 4);
        see_cap = see_buf ? w * h : 0;
    }
    if (!see_buf) return;
    px_copy(see_buf, w, bb_at(x, y), bb_stride(), w, h);
}

static void see_mix(int x, int y, int w, int h, int a) {
    if (!see_buf || w * h > see_cap) return;
    px_mix(bb_at(x, y), bb_stride(), see_buf, w, w, h, a);
    fb_mark_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

static void paint_window(int n) {
    int focus = (n == focused);
    struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    uint64_t p0 = rdtsc();
    int alpha = win_alpha(n);
    // The part of it on the screen, which is the part that is mixed.
    int sx0 = (int)nd->x, sy0 = (int)nd->y, sw0 = (int)nd->w, sh0 = (int)nd->h;
    {
        int W = (int)fb_get_width(), H = (int)fb_get_height();
        if (sx0 + sw0 > W) sw0 = W - sx0;
        if (sy0 + sh0 > H) sh0 = H - sy0;
    }
    if (alpha < 255) see_take(sx0, sy0, sw0, sh0);

    // The shadow first, under everything of the window's own; then what the
    // program shows; then the glass round it, whose rounded inner edge lies
    // over the corners of the picture. A window seen through casts less.
    uint64_t s0 = rdtsc();
    if (nd->state != WIN_MAX) {
        ui_shadow((int)nd->x, (int)nd->y, (int)nd->w, (int)nd->h, RAD_T, SHADOW,
                  (focus ? 66 : 42) * alpha / 255, SHADOW_DY, 0);
        ui_shadow((int)nd->x, (int)nd->y, (int)nd->w, (int)nd->h, RAD_T, 4,
                  (focus ? 34 : 24) * alpha / 255, 1, 0);
    }
    pf_sh += rdtsc() - s0;
    int bx, by, bw, bh;
    client_rect(n, &bx, &by, &bw, &bh);
    if (!p->gfx_on) ui_fill(bx, by, bw, bh, TH->content_bg);
    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    uint64_t c0 = rdtsc();
    draw_content(n, cx, cy, cw, ch, focus);
    pf_content += rdtsc() - c0;
    uint64_t c1 = rdtsc(), g_before = pf_gl;
    paint_chrome(n, focus);
    pf_ch += rdtsc() - c1 - (pf_gl - g_before);
    if (alpha < 255) see_mix(sx0, sy0, sw0, sh0, alpha);
    pf_paint += rdtsc() - p0;
    pf_painted++;
    pane_painted(n);
}

// Only the top of a window again: its strip, between the rounded corners.
// The glass there is drawn whole from its memory and everything else on it
// over that, so nothing old shows through.
static void paint_strip_only(int n) {
    const struct wm_node *nd = &nodes[n];
    int R = nd->state == WIN_MAX ? 0 : RAD_T;
    int sx = (int)nd->x + R, sw = (int)nd->w - 2 * R, sy = (int)nd->y;
    int saved[4];
    ui_clip_get(saved);
    ui_clip(sx, sy, sw, TITLE_H);
    paint_chrome(n, n == focused);
    ui_clip_set(saved);
    pane_painted(n);
    dmg_add(sx, sy, sx + sw, sy + TITLE_H);
}

// Only what the program shows, and the frame's glass over the body's corners.
static void paint_content_only(int n) {
    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    draw_content(n, cx, cy, cw, ch, n == focused);
    repair_body_corners(n);
    pane_painted(n);
    dmg_add((int)cx, (int)cy, (int)(cx + cw), (int)(cy + ch));
}

// Put the wallpaper back where a window's shadow and rounded corners fall,
// so they are drawn over the desktop rather than over last frame's copy of
// themselves -- a shadow laid down twice is twice as dark. The window's own
// rectangle needs nothing: glass and content cover every pixel of it.
static void restore_wall_d(int x, int y, int w, int h) {
    restore_wall(x, y, w, h);
    dmg_add(x, y, x + w, y + h);
}

static void clear_ring(int n, const int e[4]) {
    const struct wm_node *nd = &nodes[n];
    if (nd->state == WIN_MAX) return;
    int x = (int)nd->x, y = (int)nd->y, w = (int)nd->w, h = (int)nd->h;
    int y1 = e[3];
    int top = y + RAD_T, bot = y + h - RAD_B;
    if (top > y1) top = y1;
    restore_wall_d(e[0], e[1], e[2] - e[0], top - e[1]);                 // above
    if (bot < y1) restore_wall_d(e[0], bot, e[2] - e[0], y1 - bot);      // below
    int mid1 = bot < y1 ? bot : y1;
    if (mid1 > top) {
        restore_wall_d(e[0], top, x + RAD_T - e[0], mid1 - top);        // left
        restore_wall_d(x + w - RAD_T, top, e[2] - (x + w - RAD_T), mid1 - top);
    }
}

// Paint what changed, and everything that has to be painted with it.
//
// A window that changes is repainted; so is every window stacked above it,
// or the one behind would end up drawn over them. And because a window's
// shadow and corners are drawn over whatever is behind them, the wallpaper
// there is put back first -- which erases anything ELSE that was there, so
// every window whose own reach overlaps is repainted too, and so on down the
// stack until nothing more is touched.
//
// Each window's glass catches up with what is under it just before the
// window is painted: by then everything below it that changed this frame has
// been painted, and nothing above it has.
static void render_windows(void) {
    int win[MAX_NODES], vis[MAX_NODES], nv = 0;
    int n = collect_windows(cur_ws, win, MAX_NODES);
    for (int i = 0; i < n; i++)
        if (nodes[win[i]].state != WIN_MIN) vis[nv++] = win[i];

    int ext[MAX_NODES][4];
    for (int i = 0; i < nv; i++) window_extent(vis[i], ext[i]);

    // A window whose top alone changed (a button lit) or whose program's
    // picture alone did (a key typed) and that nothing above covers there is
    // painted in just that part. Everything else as before: the lowest window
    // that changed, and every window above it.
    int kind[MAX_NODES], small[MAX_NODES];
    for (int i = 0; i < nv; i++) {
        kind[i] = change_kind(vis[i]);
        small[i] = 0;
        if (kind[i] == CH_CHROME || kind[i] == CH_CONTENT) {
            const struct wm_node *nd = &nodes[vis[i]];
            int r[4];
            if (kind[i] == CH_CHROME) {
                r[0] = (int)nd->x; r[1] = (int)nd->y;
                r[2] = (int)(nd->x + nd->w); r[3] = (int)nd->y + TITLE_H;
            } else {
                uint32_t cx, cy, cw, ch;
                content_rect(vis[i], &cx, &cy, &cw, &ch);
                r[0] = (int)cx - BODY_R; r[1] = (int)cy - BODY_R;
                r[2] = (int)(cx + cw) + BODY_R; r[3] = (int)(cy + ch) + BODY_R;
            }
            small[i] = 1;
            for (int j = i + 1; j < nv; j++) if (boxes_meet(ext[j], r)) { small[i] = 0; break; }
        }
    }
    int L = nv;
    for (int i = 0; i < nv; i++) {
        if (kind[i] != CH_NONE && !small[i]) { L = i; break; }
        pf_skipped++;
    }
    for (int i = 0; i < L; i++) {
        if (!small[i]) continue;
        if (kind[i] == CH_CHROME) paint_strip_only(vis[i]);
        else paint_content_only(vis[i]);
    }
    if (L == nv) return;
    for (;;) {
        int moved = 0;
        for (int j = 0; j < L && !moved; j++)
            for (int k = L; k < nv; k++)
                if (boxes_meet(ext[j], ext[k])) { L = j; moved = 1; break; }
        if (!moved) break;
    }
    for (int k = L; k < nv; k++) {
        if (win_alpha(vis[k]) < 255) restore_wall_d(ext[k][0], ext[k][1], ext[k][2] - ext[k][0], ext[k][3] - ext[k][1]);
        else clear_ring(vis[k], ext[k]);
    }
    for (int k = L; k < nv; k++) {
        int m = vis[k];
        const struct wm_node *nd = &nodes[m];
        uint64_t k0 = rdtsc();
        ui_glass_place(&wglass[m], (int)nd->x, (int)nd->y, (int)nd->w, (int)nd->h, 16);
        glass_catch_up(&wglass[m]);
        pf_tk += rdtsc() - k0;
        paint_window(m);
        dmg_add(ext[k][0], ext[k][1], ext[k][2], ext[k][3]);
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

// What glass shows: the wallpaper, blurred. Made once per wallpaper, so a
// pane of glass costs a lookup and a blend per pixel however big it is.
static uint32_t *wp_frost;

// Which of the painted wallpapers (wall.h), when the user has not put a
// picture of their own at /wall.ppm.
static int wall_style = WALL_LAGOON;

// The system's colour comes from the wallpaper. Everything that shows it --
// the frame tint, a selection, a progress bar, programs through
// wm_palette -- is worked out from that one colour here.
static void theme_wear(uint32_t acc) {
    for (int i = 0; i < NTHEMES; i++) {
        theme_t *T = &THEMES[i];
        T->accent = acc;
        T->glow = acc;
        T->orb = acc;
        T->ui_accent = ui_mix(acc, 0, T->dark ? 0 : 40);       // white text reads on it
        T->ui_accent2 = ui_mix(acc, T->dark ? 0xFFFFFF : 0, 90);
        T->ui_sel = ui_mix(T->dark ? 0x00101418 : 0x00FFFFFF, acc, T->dark ? 90 : 60);
        T->ui_hot = ui_mix(T->dark ? 0x00101418 : 0x00FFFFFF, acc, T->dark ? 50 : 26);
        T->glass = ui_mix(T->dark ? 0x00141A20 : 0x00FFFFFF, acc, 34);
        T->glass_off = ui_mix(T->dark ? 0x00141A20 : 0x00FFFFFF, acc, 14);
    }
}

static void gen_wallpaper(uint32_t w, uint32_t h) {
    if (wallpaper) kfree(wallpaper);
    if (wp_frost) kfree(wp_frost);
    ui_set_backdrop(0, 0, 0);
    wallpaper = kmalloc((size_t)w * h * 4);
    wp_frost = kmalloc((size_t)w * h * 4);
    wp_w = wallpaper ? w : 0;
    wp_h = wallpaper ? h : 0;
    wp_theme = theme;
    if (!wallpaper) return;

    // A user picture wins over the painted one: drop a binary PPM at
    // /wall.ppm. The stick is only consulted when it is ALREADY mounted --
    // reaching for /usb here would auto-mount it during the very first repaint
    // and put SCSI traffic in every boot, which the lazy mount exists to avoid.
    if (!load_wallpaper_file("/wall.ppm", w, h) &&
        !(vol_get(0) && load_wallpaper_file("/usb/wall.ppm", w, h)))
        wall_soft(wallpaper, (int)w, (int)h, wall_style);
    theme_wear(ui_accent_from(wallpaper, (int)w, (int)h));

    if (wp_frost) {
        wall_frost(wallpaper, wp_frost, (int)w, (int)h, TH->dark);
        ui_set_backdrop(wp_frost, (int)w, (int)h);
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

    px_copy(bb_at(0, 0), bb_stride(), wallpaper, (int)w, (int)w, (int)h);
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
    if ((up / 60) % 60 < 10) sapp(ut, &q, "0");
    napp(ut, &q, (unsigned)((up / 60) % 60));
    sapp(ut, &q, ":");
    if (up % 60 < 10) sapp(ut, &q, "0");
    napp(ut, &q, (unsigned)(up % 60));
    draw_text_t(gx + gw - 12 - q * (int)GW, yy, ut, 0x00B8C0CC);
}

// --- the island ---------------------------------------------------------------
//
// The panel, floating: a rounded slab of coloured glass, clear of the screen's
// edge, holding the launcher (the xyuOS mark), the programs -- the ones kept
// on it and the ones running -- and a pill with the network, the sound, the
// time and the date. It sits at the bottom by default and can be dragged to
// the left, right or top edge, where it turns to fit.
//
// Where everything sits is worked out in ONE place, tb_layout(), and the
// drawing, the hit-testing and the hover all read it.

static int start_open;                 // the launcher (defined further down)
static int cc_open;                    // the control panel (likewise)
static int am_open;                    // a program's windows, from the island
static uint64_t pop_t0;                // when the open one came up, for its spring
static int geom_rest;                  // 1: where a popup rests, without the spring

#define ISL_BOTTOM 0
#define ISL_LEFT   1
#define ISL_RIGHT  2
#define ISL_TOP    3
static int island_edge = ISL_BOTTOM;
static int island_edge_now(void) { return island_edge; }

#define TB_ORB    1                    // the launcher
#define TB_APP    2                    // a program: kept, running, or both
#define TB_STATUS 3                    // network, sound, time
#define TB_MAX    (MAX_NODES + 16)

#define ISL_PAD    10
#define ISL_ICON   44
#define ISL_SLOT   52
#define ISL_GAP    14
#define ISL_STAT_W 204                 // the status pill, lying down
#define ISL_STAT_H 138                 //                  standing up
#define ISL_R      26

struct tb_item {
    int kind, x, y, w, h;
    int win;                           // TB_APP: its window, or -1 if not running
    int nwin;                          // TB_APP: how many windows it has
    char name[16];                     // TB_APP: the program's file in /bin
};
static struct tb_item tb_items[TB_MAX];
static int tb_n;
static int isl_x, isl_y, isl_w, isl_h;

// The programs kept on the island, whether running or not.
static const char *const isl_kept[] = { "files", "web", "sh", "note", "play", "taskmgr", "control" };
#define ISL_KEPT (int)(sizeof isl_kept / sizeof isl_kept[0])

static int isl_vertical(void) { return island_edge == ISL_LEFT || island_edge == ISL_RIGHT; }

static void str_put(char *d, const char *s, int max) {
    int i = 0;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int str_same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void tb_layout(void) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    int vertical = isl_vertical();
    tb_n = 0;

    // The slots: kept programs first, then any other program with a window.
    int win[MAX_NODES];
    int nw = collect_windows_ex(cur_ws, win, MAX_NODES, 1);
    struct tb_item apps[TB_MAX];
    int na = 0;
    for (int k = 0; k < ISL_KEPT; k++) {
        struct tb_item *a = &apps[na++];
        a->kind = TB_APP;
        str_put(a->name, isl_kept[k], sizeof a->name);
        a->win = -1;
        a->nwin = 0;
    }
    // Oldest window first, so a program's place does not shuffle on a click.
    for (int i = 0; i < MAX_NODES; i++) {
        int n = -1;
        for (int j = 0; j < nw; j++) if (win[j] == i) n = i;
        if (n < 0) continue;
        const char *t = pane_title(&panes[nodes[n].pane_idx]);
        int s = -1;
        for (int k = 0; k < na; k++) if (str_same(apps[k].name, t)) s = k;
        if (s < 0 && na < TB_MAX - 2) {
            s = na++;
            apps[s].kind = TB_APP;
            str_put(apps[s].name, t, sizeof apps[s].name);
            apps[s].win = -1;
            apps[s].nwin = 0;
        }
        if (s < 0) continue;
        apps[s].nwin++;
        // The one it brings forward: the focused window, else the newest.
        if (apps[s].win < 0 || n == focused) apps[s].win = n;
    }

    int along = ISL_PAD + ISL_ICON + ISL_GAP + na * ISL_SLOT + ISL_GAP +
                (vertical ? ISL_STAT_H : ISL_STAT_W) + ISL_PAD;
    if (vertical) {
        isl_w = ISLAND_H;
        isl_h = along;
        isl_x = island_edge == ISL_LEFT ? 14 : W - 14 - ISLAND_H;
        isl_y = (H - along) / 2;
    } else {
        isl_w = along;
        isl_h = ISLAND_H;
        isl_x = (W - along) / 2;
        isl_y = island_edge == ISL_TOP ? 14 : H - 14 - ISLAND_H;
    }

    // Lay them along it.
    int at = ISL_PAD;
    struct tb_item *o = &tb_items[tb_n++];
    o->kind = TB_ORB; o->win = -1; o->nwin = 0; o->name[0] = 0;
    if (vertical) { o->x = isl_x + (ISLAND_H - ISL_ICON) / 2; o->y = isl_y + at; }
    else          { o->x = isl_x + at; o->y = isl_y + (ISLAND_H - ISL_ICON) / 2; }
    o->w = o->h = ISL_ICON;
    at += ISL_ICON + ISL_GAP;
    for (int k = 0; k < na; k++) {
        struct tb_item *it = &tb_items[tb_n++];
        *it = apps[k];
        if (vertical) { it->x = isl_x + (ISLAND_H - ISL_SLOT) / 2; it->y = isl_y + at; }
        else          { it->x = isl_x + at; it->y = isl_y + (ISLAND_H - ISL_SLOT) / 2; }
        it->w = it->h = ISL_SLOT;
        at += ISL_SLOT;
    }
    at += ISL_GAP;
    struct tb_item *s = &tb_items[tb_n++];
    s->kind = TB_STATUS; s->win = -1; s->nwin = 0; s->name[0] = 0;
    if (vertical) { s->x = isl_x + 8; s->y = isl_y + at; s->w = ISLAND_H - 16; s->h = ISL_STAT_H; }
    else          { s->x = isl_x + at; s->y = isl_y + 10; s->w = ISL_STAT_W; s->h = ISLAND_H - 20; }
}

// Where the layout's little label sits in the status pill.
#define LANG_CHIP_W 30
#define LANG_CHIP_H 20
static void lang_chip_at(const struct tb_item *it, int *x, int *y) {
    if (it->w > it->h) { *x = it->x + 70; *y = it->y + (it->h - LANG_CHIP_H) / 2; }
    else { *x = it->x + (it->w - LANG_CHIP_W) / 2; *y = it->y + 64; }
}

static int tb_item_at(int mx, int my) {
    for (int i = 0; i < tb_n; i++)
        if (mx >= tb_items[i].x && mx < tb_items[i].x + tb_items[i].w &&
            my >= tb_items[i].y && my < tb_items[i].y + tb_items[i].h) return i;
    return -1;
}

static int on_island(int mx, int my) {
    return mx >= isl_x && mx < isl_x + isl_w && my >= isl_y && my < isl_y + isl_h;
}

// Everything the island shows, folded into one value: unchanged, it is not
// drawn again.
static uint64_t taskbar_fingerprint(void) {
    uint64_t hsh = 14695981039346656037ULL;
    hsh = (hsh ^ (uint64_t)theme) * FNV_P;
    hsh = (hsh ^ (uint64_t)island_edge) * FNV_P;
    hsh = (hsh ^ (uint64_t)focused) * FNV_P;
    hsh = (hsh ^ (uint64_t)start_open) * FNV_P;
    hsh = (hsh ^ (uint64_t)(cc_open + 2)) * FNV_P;
    hsh = (hsh ^ (uint64_t)(press_key + 7)) * FNV_P;
    hsh = (hsh ^ (uint64_t)TH->accent) * FNV_P;
    for (int i = 0; i < tb_n; i++) {
        const struct tb_item *it = &tb_items[i];
        hsh = (hsh ^ (uint64_t)(it->kind * 131 + it->x * 7 + it->y * 3 + it->w)) * FNV_P;
        hsh = (hsh ^ ((uint64_t)(it->win + 1) * 17 + (uint64_t)it->nwin)) * FNV_P;
        hsh = (hsh ^ (uint64_t)glow_of(KEY_TB(i))) * FNV_P;
        for (int k = 0; it->name[k]; k++) hsh = (hsh ^ (uint8_t)it->name[k]) * FNV_P;
        if (it->win >= 0) hsh = (hsh ^ (uint64_t)nodes[it->win].state) * FNV_P;
    }
    hsh = (hsh ^ (uint64_t)net_is_up()) * FNV_P;
    hsh = (hsh ^ (uint64_t)audio_ready()) * FNV_P;
    hsh = (hsh ^ (uint64_t)audio_volume()) * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.hour) * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.min)  * FNV_P;
    hsh = (hsh ^ (uint64_t)wm_clock.day)  * FNV_P;
    hsh = (hsh ^ (uint64_t)(kbd_ru + 5)) * FNV_P;
    return hsh;
}

static uint64_t tb_fp;
static int      tb_valid = 0;

// --- keeping what is under a floating thing ------------------------------------
//
// The island, a menu, a notice: each floats over the windows, and its shadow
// and rounded corners are blended with whatever is under it. Drawn again in
// place it would blend with its own last copy and darken a little more each
// time. So each keeps the exact pixels under it -- refreshed only where
// something under it was painted this frame -- and puts them back before it
// is drawn again.
typedef struct { int x, y, w, h, cap, have; uint32_t *px; } ukeep;

// Whether placing k at (x, y, w, h) would leave it where it is.
static int uk_same(const ukeep *k, int x, int y, int w, int h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return k->x == x && k->y == y && k->w == w && k->h == h;
}

static void uk_place(ukeep *k, int x, int y, int w, int h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (k->have && k->x == x && k->y == y && k->w == w && k->h == h) return;
    k->x = x; k->y = y; k->w = w; k->h = h;
    k->have = 0;
    if (w * h > k->cap) {
        if (k->px) kfree(k->px);
        k->px = (uint32_t *)kmalloc((size_t)w * h * 4);
        k->cap = k->px ? w * h : 0;
    }
}

static void uk_copy_in(ukeep *k, int x0, int y0, int x1, int y1) {
    if (x0 < k->x) x0 = k->x;
    if (y0 < k->y) y0 = k->y;
    if (x1 > k->x + k->w) x1 = k->x + k->w;
    if (y1 > k->y + k->h) y1 = k->y + k->h;
    if (x1 <= x0 || y1 <= y0) return;
    px_copy(k->px + (size_t)(y0 - k->y) * k->w + (x0 - k->x), k->w, bb_at(x0, y0), bb_stride(),
            x1 - x0, y1 - y0);
}

// Take in what is under, wherever this frame painted it.
static void uk_take(ukeep *k) {
    if (!k->cap) return;
    if (!k->have || dmg_all) {
        uk_copy_in(k, k->x, k->y, k->x + k->w, k->y + k->h);
        k->have = 1;
        return;
    }
    for (int i = 0; i < ndmg; i++) uk_copy_in(k, dmg[i][0], dmg[i][1], dmg[i][2], dmg[i][3]);
}

// Mix what was drawn over the keeper's piece with what it keeps: a = 256 is
// all the new picture, 0 none of it.
static void uk_fade(ukeep *k, int a) {
    if (!k->cap || !k->have || a >= 256) return;
    px_mix(bb_at(k->x, k->y), bb_stride(), k->px, k->w, k->w, k->h, a);
    fb_mark_rect((uint32_t)k->x, (uint32_t)k->y, (uint32_t)k->w, (uint32_t)k->h);
}

static void uk_restore(ukeep *k) {
    if (!k->cap || !k->have) return;
    px_copy(bb_at(k->x, k->y), bb_stride(), k->px, k->w, k->w, k->h);
    fb_mark_rect((uint32_t)k->x, (uint32_t)k->y, (uint32_t)k->w, (uint32_t)k->h);
}

// Whether anything this frame painted inside (x, y, w, h).
static int dmg_meets(int x, int y, int w, int h) {
    if (dmg_all) return 1;
    int r[4] = { x, y, x + w, y + h };
    for (int i = 0; i < ndmg; i++) if (boxes_meet(dmg[i], r)) return 1;
    return 0;
}

static ukeep isl_keep;
static ui_glass isl_glass;

// The keeper's reach: the island, its shadow, and room on the inner side for
// the name that appears over a program under the pointer.
static void island_reach(int *x, int *y, int *w, int *h) {
    int m = 40, tip = 46;
    *x = isl_x - m; *y = isl_y - m; *w = isl_w + 2 * m; *h = isl_h + 2 * m;
    switch (island_edge) {
    case ISL_BOTTOM: *y -= tip; *h += tip; break;
    case ISL_TOP:    *h += tip; break;
    case ISL_LEFT:   *w += 160; break;
    default:         *x -= 160; *w += 160; break;
    }
}

static void two_digits(char *b, int v) { b[0] = (char)('0' + (v / 10) % 10); b[1] = (char)('0' + v % 10); }

static const char *const wdays_s[2][7] = {
    { "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" },
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" } };
static const char *const months_s[2][12] = {
    { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" },
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" } };

static int dow(int y, int m, int d) {          // 0 = Sunday
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

// The accent as a colour for text and small marks on the glass: a shade
// darker on the light theme's white, a shade lighter on the dark one's slate.
static uint32_t accent_ink(const theme_t *T, int k) {
    return T->dark ? ui_mix(T->accent, 0x00FFFFFF, k) : ui_mix(T->accent, 0, k);
}

// A program's name as a person reads it, for the name over its icon.
static const char *program_label(const char *file) {
    static const struct { const char *f, *n, *en; } names[] = {
        { "files", "Файлы", "Files" }, { "web", "Браузер", "Browser" },
        { "sh", "Терминал", "Terminal" }, { "note", "Блокнот", "Notepad" },
        { "play", "Музыка", "Music" }, { "taskmgr", "Диспетчер задач", "Task manager" },
        { "control", "Настройки", "Settings" }, { "devmgr", "Устройства", "Devices" },
        { "view", "Просмотр", "Viewer" }, { "netsurf", "NetSurf", "NetSurf" },
        { "doom", "DOOM", "DOOM" }, { "python", "Python", "Python" },
        { "cc", "Компилятор C", "C compiler" }, { "edit", "Редактор", "Editor" },
        { "fm", "Файлы (текст)", "Files (text)" },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (str_same(names[i].f, file)) return L(names[i].n, names[i].en);
    return file;
}

static void draw_island_now(void) {
    const theme_t *T = TH;
    int vertical = isl_vertical();
    int x = isl_x, y = isl_y, w = isl_w, h = isl_h;

    ui_shadow(x, y, w, h, ISL_R, 30, 70, 10, 0);
    ui_shadow(x, y, w, h, ISL_R, 4, 34, 1, 0);
    ui_glass_place(&isl_glass, x, y, w, h, 16);
    ui_glass_forget(&isl_glass);
    ui_glass_take(&isl_glass, 0, 0);
    ui_glass_draw(&isl_glass, x, y, w, h, ISL_R, 0, 0, 0, 0, 0, 0,
                  ui_mix(T->frost, T->accent, 60), 120 + T->frost_a);
    ui_glass_rim(x, y, w, h, ISL_R);

    int hot = -1;
    for (int i = 0; i < tb_n; i++) {
        const struct tb_item *it = &tb_items[i];
        int glow = glow_of(KEY_TB(i));
        int pressed = press_key == KEY_TB(i) && glow;
        if (glow > 128) hot = i;
        switch (it->kind) {
        case TB_ORB: {
            int R = ISL_ICON / 2;
            if (start_open || glow)
                ui_round_fill(it->x - 3, it->y - 3, ISL_ICON + 6, ISL_ICON + 6, R + 3, T->plate,
                              start_open ? 200 : glow * 160 / 256);
            glyph_logo(it->x + R, it->y + R + (pressed ? 1 : 0), R - (pressed ? 1 : 0), T->accent);
            break;
        }
        case TB_APP: {
            int active = it->win >= 0 && it->win == focused && nodes[it->win].state != WIN_MIN;
            if (active || glow) {
                int a = active ? 150 : 0;
                a += glow * 90 / 256;
                if (a > 230) a = 230;
                ui_round_fill(it->x + 1, it->y + 1, ISL_SLOT - 2, ISL_SLOT - 2, 16, T->plate, a);
            }
            int ix = it->x + (ISL_SLOT - ISL_ICON) / 2, iy = it->y + (ISL_SLOT - ISL_ICON) / 2;
            if (pressed) iy++;
            if (!ui_icon(ix + (ISL_ICON - ICON_BIG) / 2, iy + (ISL_ICON - ICON_BIG) / 2, it->name, ICON_BIG)) {
                int g;
                uint32_t col;
                glyph_for_program(it->name, &g, &col);
                glyph_app(ix, iy, ISL_ICON, col, g);
            }
            // Running: a dot by it -- a longer one for the window in front.
            if (it->nwin) {
                int len = active ? 14 : 5;
                uint32_t c = active ? T->accent : T->title_text_dim;
                switch (island_edge) {
                case ISL_BOTTOM: ui_round_fill(it->x + ISL_SLOT / 2 - len / 2, y + h - 7, len, 4, 2, c, 255); break;
                case ISL_TOP:    ui_round_fill(it->x + ISL_SLOT / 2 - len / 2, y + 3, len, 4, 2, c, 255); break;
                case ISL_LEFT:   ui_round_fill(x + 3, it->y + ISL_SLOT / 2 - len / 2, 4, len, 2, c, 255); break;
                default:         ui_round_fill(x + w - 7, it->y + ISL_SLOT / 2 - len / 2, 4, len, 2, c, 255); break;
                }
            }
            break;
        }
        case TB_STATUS: {
            ui_round_fill(it->x, it->y, it->w, it->h, vertical ? 22 : it->h / 2, T->plate,
                          cc_open ? 235 : 110 + glow * 100 / 256);
            uint32_t ink = T->title_text, dim = T->title_text_dim;
            int up = net_is_up(), snd = audio_ready() > 0;
            char tm[6];
            two_digits(tm, wm_clock.hour); tm[2] = ':'; two_digits(tm + 3, wm_clock.min); tm[5] = 0;
            int lx, ly;
            lang_chip_at(it, &lx, &ly);
            ui_round_fill(lx, ly, LANG_CHIP_W, LANG_CHIP_H, 6, ink, 34);
            const char *lc = kbd_ru ? "RU" : "EN";
            ui_text(lx + (LANG_CHIP_W - ui_text_w(lc, UI_F11)) / 2,
                    ly + (LANG_CHIP_H - ui_line_h(UI_F11)) / 2, lc, UI_F11, ink);
            if (!vertical) {
                glyph_draw(G_WIFI, it->x + 14, it->y + 13, 18, 0xFF000000 | (up ? ink : dim), 0);
                glyph_draw(G_VOL, it->x + 42, it->y + 13, 18, 0xFF000000 | (snd ? ink : dim), 0);
                char dt[24];
                int q = 0;
                const char *wd = wdays_s[lang][dow(wm_clock.year, wm_clock.mon, wm_clock.day)];
                while (*wd) dt[q++] = *wd++;
                dt[q++] = ','; dt[q++] = ' ';
                if (wm_clock.day >= 10) dt[q++] = (char)('0' + wm_clock.day / 10);
                dt[q++] = (char)('0' + wm_clock.day % 10);
                dt[q++] = ' ';
                const char *mo = months_s[lang][(wm_clock.mon + 11) % 12];
                while (*mo) dt[q++] = *mo++;
                dt[q] = 0;
                ui_text(it->x + 108, it->y + 3, tm, UI_F15B, ink);
                ui_text(it->x + 108, it->y + 23, dt, UI_F11, dim);
            } else {
                int cx = it->x + it->w / 2;
                glyph_draw(G_WIFI, cx - 10, it->y + 10, 20, 0xFF000000 | (up ? ink : dim), 0);
                glyph_draw(G_VOL, cx - 10, it->y + 38, 20, 0xFF000000 | (snd ? ink : dim), 0);
                char hh[3] = { tm[0], tm[1], 0 }, mm[3] = { tm[3], tm[4], 0 };
                ui_text(cx - ui_text_w(hh, UI_F15B) / 2, it->y + 90, hh, UI_F15B, ink);
                ui_text(cx - ui_text_w(mm, UI_F15B) / 2, it->y + 110, mm, UI_F15B, ink);
            }
            break;
        }
        }
    }

    // The name of the program under the pointer, beside the island -- not
    // while a popup stands there, where it would peek out from under it.
    if (hot >= 0 && tb_items[hot].kind == TB_APP && !start_open && !cc_open && !am_open) {
        const struct tb_item *it = &tb_items[hot];
        const char *label = program_label(it->name);
        int tw = ui_text_w(label, UI_F13) + 24, th = 30;
        int lx, ly;
        switch (island_edge) {
        case ISL_BOTTOM: lx = it->x + ISL_SLOT / 2 - tw / 2; ly = y - th - 10; break;
        case ISL_TOP:    lx = it->x + ISL_SLOT / 2 - tw / 2; ly = y + h + 10; break;
        case ISL_LEFT:   lx = x + w + 10; ly = it->y + ISL_SLOT / 2 - th / 2; break;
        default:         lx = x - tw - 10; ly = it->y + ISL_SLOT / 2 - th / 2; break;
        }
        ui_shadow(lx, ly, tw, th, 15, 10, 50, 3, 0);
        ui_round_fill(lx, ly, tw, th, 15, T->plate, 240);
        ui_text(lx + 12, ly + (th - ui_line_h(UI_F13)) / 2, label, UI_F13, T->title_text);
    }
}

// Draw the island if anything about it, or anything under it, changed.
// Returns 1 if it did.
static int draw_taskbar(void) {
    tb_layout();
    uint64_t fp = taskbar_fingerprint();
    int rx, ry, rw, rh;
    island_reach(&rx, &ry, &rw, &rh);
    int under = dmg_meets(rx, ry, rw, rh);
    if (tb_valid && tb_fp == fp && !under) return 0;   // identical to what is on screen
    tb_fp = fp;
    tb_valid = 1;

    // Grown or shrunk -- a program came or went, a terminal is running
    // something under another name -- or moved: what was under the old one
    // goes back first, or its ends stay on the screen.
    if (isl_keep.have && !uk_same(&isl_keep, rx, ry, rw, rh)) {
        uk_take(&isl_keep);
        uk_restore(&isl_keep);
        dmg_add(isl_keep.x, isl_keep.y, isl_keep.x + isl_keep.w, isl_keep.y + isl_keep.h);
    }
    uk_place(&isl_keep, rx, ry, rw, rh);
    uk_take(&isl_keep);
    uk_restore(&isl_keep);
    draw_island_now();
    dmg_add(rx, ry, rx + rw, ry + rh);
    return 1;
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

    // Something new started playing: say what.
    {
        static char last_title[AUDIO_TITLE_MAX];
        const char *t = audio_title();
        if (t[0] && !str_same(t, last_title)) {
            str_put(last_title, t, sizeof last_title);
            wm_notify_glyph(L("Сейчас играет", "Now playing"), t, G_MUSIC, 0x00E06A4C);
        } else if (!t[0]) {
            last_title[0] = 0;
        }
    }

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

// Where a dragged window would land: a tinted pane with an accent outline,
// the same affordance Windows shows when you shove a window at an edge.
static void draw_snap_preview(void) {
    if (snap_hint == SNAP_NONE) return;
    const theme_t *T = TH;
    uint32_t x, y, w, h;
    snap_rect(snap_hint, &x, &y, &w, &h);
    // Where the window will land: a soft plate of the accent, inset a little.
    ui_round_fill((int)x + 8, (int)y + 8, (int)w - 16, (int)h - 16, 22, T->accent, 60);
    ui_rrect_line((int)x + 8, (int)y + 8, (int)w - 16, (int)h - 16, 22, UI_ALL, T->accent, 200);
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
    const theme_t *T = TH;

    int w = ui_text_w(udrag_label, UI_F13) + 28, h = 32;
    int x = mx + 16, y = my + 16;
    if (x + w > (int)fb_get_width())  x = (int)fb_get_width() - w;
    if (y + h > (int)fb_get_height()) y = (int)fb_get_height() - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    ui_shadow(x, y, w, h, 16, 12, 60, 4, 0);
    ui_round_fill(x, y, w, h, 16, T->plate, 240);
    ui_text(x + 14, y + (h - ui_line_h(UI_F13)) / 2, udrag_label, UI_F13, T->title_text);
}

// --- message boxes --------------------------------------------------------
//
// A small queue rather than a single slot: a crashing program can fault more
// than once on its way down, and the second report must not overwrite the
// first one before anybody has read it.

#define MBOX_QUEUE  4
#define MBOX_TEXT   160         // UTF-8: a Russian letter is two bytes
#define MBOX_CODE   24

struct mbox {
    int  used, kind;
    char code[MBOX_CODE];
    char title[96];
    char text[MBOX_TEXT];
    char detail[MBOX_TEXT];
};

static struct mbox mboxes[MBOX_QUEUE];
static int mbox_head;              // the one being shown

static void mb_copy(char *d, const char *s2, int max) {
    int i = 0;
    if (s2) while (s2[i] && i < max - 1) { d[i] = s2[i]; i++; }
    // Cut short, never in the middle of a letter: back off a partial UTF-8
    // sequence rather than show half of one as a box.
    if (s2 && s2[i]) {
        int j = i;
        while (j > 0 && ((unsigned char)d[j - 1] & 0xC0) == 0x80) j--;
        if (j > 0 && ((unsigned char)d[j - 1] & 0xC0) == 0xC0) i = j - 1;
    }
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

static void mbox_geom(int *x, int *y, int *w, int *h) {
    // Sized to the longest line rather than to a guess. A fixed width is fine
    // until the first message that does not fit, and then it is wrong in the
    // one situation where the text matters most.
    struct mbox *m = &mboxes[mbox_head];
    int tw = ui_text_w(m->title[0] ? m->title : "Error", UI_F15B);
    int a = ui_text_w(m->text, UI_F13), d = ui_text_w(m->detail, UI_F12);
    if (a > tw) tw = a;
    if (d > tw) tw = d;
    *w = tw + 92 + 36;
    if (*w < 440) *w = 440;
    if (*w > (int)fb_get_width() - 80) *w = (int)fb_get_width() - 80;
    *h = 196;
    *x = ((int)fb_get_width() - *w) / 2;
    *y = ((int)fb_get_height() - *h) / 2 - 40;
    if (*y < 20) *y = 20;
}

static void mbox_ok_rect(int *x, int *y, int *w, int *h) {
    int dx, dy, dw, dh;
    mbox_geom(&dx, &dy, &dw, &dh);
    *w = 104;
    *h = 38;
    *x = dx + dw - *w - 22;
    *y = dy + dh - *h - 20;
}

// The icon carries the severity before a single word is read, which is the
// entire reason message boxes have one.
// The sign of a message: a soft square in its colour, with a mark in it.
static uint32_t mbox_colour(int kind) {
    return kind == MB_ERROR ? 0x00D9534F : kind == MB_WARN ? 0x00E0A030 : TH->accent;
}

static void draw_message_box(void) {
    struct mbox *m = &mboxes[mbox_head];
    if (!m->used) return;
    const theme_t *T = TH;

    int x, y, w, h;
    mbox_geom(&x, &y, &w, &h);

    // The desktop goes quiet behind it: this is modal, and it should look it.
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (uint32_t yy = 0; yy < fb_get_height(); yy++) {
        volatile uint32_t *row = (volatile uint32_t *)(base + (size_t)yy * pitch);
        for (uint32_t xx = 0; xx < fb_get_width(); xx++)
            row[xx] = mix(row[xx], 0x00000000, 70);
    }
    fb_mark_rows(0, fb_get_height());

    ui_shadow(x, y, w, h, 24, 40, 90, 16, 0);
    ui_glass_live(x, y, w, h, 24, ui_mix(T->frost, T->accent, 30), 215 + T->frost_a / 2, 12);
    ui_glass_rim(x, y, w, h, 24);

    uint32_t ink = T->title_text, dim = T->title_text_dim;
    glyph_app(x + 26, y + 28, 50, mbox_colour(m->kind), m->kind == MB_ERROR ? G_CLOSE : G_BELL);
    int tx = x + 96, tw = w - 96 - 24;
    ui_text_fit(tx, y + 24, tw, m->title[0] ? m->title : L("Ошибка", "Error"), UI_F15B, ink);
    ui_text_fit(tx, y + 54, tw, m->text, UI_F13, ink);
    if (m->detail[0]) ui_text_fit(tx, y + 80, tw, m->detail, UI_F12, dim);
    if (m->code[0]) {
        char line[MBOX_CODE + 16];
        int n = 0;
        const char *pre = L("код ", "code ");
        while (pre[n]) { line[n] = pre[n]; n++; }
        int k = 0;
        while (m->code[k] && n < (int)sizeof line - 1) line[n++] = m->code[k++];
        line[n] = 0;
        ui_text_fit(tx, y + 104, tw, line, UI_F11, dim);
    }

    int bx, by, bw, bh;
    mbox_ok_rect(&bx, &by, &bw, &bh);
    ui_round_fill(bx, by, bw, bh, bh / 2, T->accent, 255);
    const char *ok = L("Понятно", "OK");
    ui_text(bx + (bw - ui_text_w(ok, UI_F13B)) / 2, by + (bh - ui_line_h(UI_F13B)) / 2, ok, UI_F13B,
            0x00FFFFFF);
}

// --- the start menu -------------------------------------------------------
//
// Two lists in one: a short curated shelf of the programs a person actually
// launches, and everything else in /bin behind the search box. The catalogue
// is rebuilt each time the menu opens rather than cached, because /bin is a
// directory like any other -- a program compiled a minute ago with `cc` shows
// up without anything having to be told about it.

#define START_MAX  48
#define START_NAME 48          // a Russian name is two bytes a letter
#define START_DESC 40

struct start_item {
    char name[START_NAME];
    char path[48];
    char desc[START_DESC];
    // The file in /bin this came from. The display name may be prettier
    // ("Task manager"), but the icon was drawn against the file name.
    char name_file[START_NAME];
    char alt[START_NAME];          // its English name, which search also knows
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
static const struct { const char *file, *name, *en, *desc; } start_known[] = {
    { "web",     "Браузер",         "Browser",        "Open a page on the web" },
    { "netsurf", "NetSurf",         "NetSurf",        "The NetSurf browser, ported here" },
    { "files",   "Файлы",           "Files",          "Browse folders and open things" },
    { "note",    "Блокнот",         "Notepad",        "Write and edit text" },
    { "view",    "Просмотр",        "Viewer",         "Pictures and text files" },
    { "taskmgr", "Диспетчер задач", "Task manager",   "Processes, CPU and memory" },
    { "control", "Настройки",       "Settings",       "Theme, keyboard, mouse, power" },
    { "devmgr",  "Устройства",      "Devices",        "What is in this machine" },
    { "sh",      "Терминал",        "Terminal",       "The command shell" },
    { "play",    "Музыка",          "Music",          "Play WAV, MP3 and Ogg" },
    { "doom",    "DOOM",            "DOOM",           "It runs DOOM" },
    { "plasma",  "Плазма",          "Plasma",         "A graphics demo" },
    { "python",  "Python",          "Python",         "An interactive prompt, or run a script" },
    { "cc",      "Компилятор C",    "C compiler",     "Build a program on the machine itself" },
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
    str_cpy(it->alt, file, START_NAME);
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
        str_cpy(it->name, L(start_known[i].name, start_known[i].en), START_NAME);
        str_cpy(it->alt, L(start_known[i].en, start_known[i].name), START_NAME);
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

// --- the launcher -------------------------------------------------------------
//
// It rises out of the island, by the mark: a search field; the programs kept
// in it, in a grid, or all of them by letter; what was opened lately; and at
// the foot you, the settings and the power button. Typing searches. It is
// coloured glass like the island, and keeps what is under it the same way.
//
// Geometry is worked out in one place for drawing and clicking alike.

#define LN_W      640
#define LN_H      560
#define LN_ROW    40
#define LN_HEAD   30
#define LN_CELL_W 98
#define LN_CELL_H 100

// One line of a list: a program, or the letter that heads a run of them.
#define SM_ROWS_MAX (START_MAX + 30)
static struct { int item; char letter; } sm_rows[SM_ROWS_MAX];
static int sm_nrows;
static int sm_power_open;           // the restart / shut down choice is showing
static int sm_all;                  // all programs, rather than the kept ones
static int menu_dirty;              // the menu, and only the menu, changed

// The programs kept in the launcher's grid, by their file in /bin.
static const char *const ln_pins[12] = {
    "files", "web", "sh", "note", "play", "view",
    "taskmgr", "control", "python", "doom", "devmgr", "cc",
};

// What was opened lately: a program, and what it was given to open.
#define RECENT_MAX 6
static struct { char path[48]; char arg[96]; int hh, mm; } recents[RECENT_MAX];
static int recent_n;

static void recent_note(const char *path, const char *arg) {
    if (!path) return;
    int at = -1;
    for (int i = 0; i < recent_n; i++)
        if (str_same(recents[i].path, path) && str_same(recents[i].arg, arg ? arg : "")) at = i;
    if (at < 0) at = recent_n < RECENT_MAX ? recent_n++ : RECENT_MAX - 1;
    for (int i = at; i > 0; i--) recents[i] = recents[i - 1];
    str_put(recents[0].path, path, sizeof recents[0].path);
    str_put(recents[0].arg, arg ? arg : "", sizeof recents[0].arg);
    recents[0].hh = wm_clock.hour;
    recents[0].mm = wm_clock.min;
}

static int ci_cmp(const char *a, const char *b) {
    while (*a && lower_c(*a) == lower_c(*b)) { a++; b++; }
    return (int)(unsigned char)lower_c(*a) - (int)(unsigned char)lower_c(*b);
}

// How well a program matches the search, smaller is better, -1 not at all:
// its name starts with it, a word of its name does, the name has it
// somewhere, only its other name or file does. "fi" finds Files first.
static int name_rank(const char *n) {
    int k = 0;
    while (start_q[k] && lower_c(n[k]) == lower_c(start_q[k])) k++;
    if (!start_q[k]) return 0;
    for (int i = 1; n[i]; i++) {
        if (n[i - 1] != ' ') continue;
        k = 0;
        while (start_q[k] && lower_c(n[i + k]) == lower_c(start_q[k])) k++;
        if (!start_q[k]) return 1;
    }
    if (str_has(n, start_q)) return 2;
    return -1;
}

// Both names count the same -- typing on a Latin keyboard, "fi" means Files
// as much as it means anything spelled with those letters.
static int sm_rank(const struct start_item *it) {
    int a = name_rank(it->name), b = name_rank(it->alt);
    int r = a < 0 ? b : (b < 0 ? a : (a < b ? a : b));
    if (r < 0) return str_has(it->path, start_q) ? 6 : -1;
    return r * 2 + (it->featured ? 0 : 1);
}

// The list for the current search -- every program it matches, best first
// -- or, with none, the programs worth naming under their initials.
static void sm_build_rows(void) {
    int idx[START_MAX], rank[START_MAX], n = 0;
    for (int i = 0; i < start_count; i++) {
        int r = 0;
        if (start_qlen) {
            r = sm_rank(&start_items[i]);
            if (r < 0) continue;
        } else if (!start_items[i].featured) {
            continue;
        }
        rank[i] = r;
        idx[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && (rank[idx[j]] > rank[v] ||
                          (rank[idx[j]] == rank[v] &&
                           ci_cmp(start_items[idx[j]].name, start_items[v].name) > 0))) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    sm_nrows = 0;
    int last = -1;
    for (int i = 0; i < n && sm_nrows < SM_ROWS_MAX - 1; i++) {
        // The heading is the first letter -- a whole UTF-8 sequence for a
        // Russian name, so the first byte is enough to tell runs apart.
        unsigned char c = (unsigned char)start_items[idx[i]].name[0];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (!start_qlen && c != last) {
            sm_rows[sm_nrows].item = -1;
            sm_rows[sm_nrows].letter = (char)idx[i];   // the program whose initial it shows
            sm_nrows++;
            last = c;
        }
        sm_rows[sm_nrows].item = idx[i];
        sm_rows[sm_nrows].letter = 0;
        sm_nrows++;
    }
}

static int sm_first_item(void) {
    for (int r = 0; r < sm_nrows; r++) if (sm_rows[r].item >= 0) return r;
    return -1;
}

static void sm_step(int d) {
    int r = start_sel;
    for (;;) {
        r += d;
        if (r < 0 || r >= sm_nrows) return;
        if (sm_rows[r].item >= 0) { start_sel = r; return; }
    }
}

// Where the launcher is: by the mark on the island, on its inner side.
// A popup coming up starts a little short of its place, on the island's side,
// and springs into it.
#define POP_TRAVEL 28
static void pop_shift(int *x, int *y) {
    if (geom_rest) return;
    int d = (256 - spring_at(now_ms() - pop_t0)) * POP_TRAVEL / 256;
    switch (island_edge) {
    case ISL_LEFT:  *x -= d; break;
    case ISL_RIGHT: *x += d; break;
    case ISL_TOP:   *y -= d; break;
    default:        *y += d; break;
    }
}

static void start_geom(int *x, int *y, int *w, int *h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    tb_layout();
    const struct tb_item *o = &tb_items[0];
    *w = LN_W;
    *h = LN_H;
    if (*h > H - 2 * TASKBAR_H) *h = H - 2 * TASKBAR_H;
    switch (island_edge) {
    case ISL_LEFT:   *x = isl_x + isl_w + 14; *y = o->y - 12; break;
    case ISL_RIGHT:  *x = isl_x - 14 - *w;    *y = o->y - 12; break;
    case ISL_TOP:    *x = o->x - 12;          *y = isl_y + isl_h + 14; break;
    default:         *x = o->x - 12;          *y = isl_y - 14 - *h; break;
    }
    if (*x < 8) *x = 8;
    if (*x + *w > W - 8) *x = W - 8 - *w;
    if (*y < 8) *y = 8;
    if (*y + *h > H - 8) *y = H - 8 - *h;
    pop_shift(x, y);
}

// The list's box, in the all-programs and search views.
static void sm_list_box(int *lx, int *ly, int *lw, int *lh) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    *lx = x + 16;
    *ly = y + (start_qlen ? 84 : 120);
    *lw = w - 32;
    *lh = y + h - 72 - *ly;
}

static int sm_row_height(int r) { return sm_rows[r].item < 0 ? LN_HEAD : LN_ROW; }

static void sm_scroll_to_sel(void) {
    int lx, ly, lw, lh;
    sm_list_box(&lx, &ly, &lw, &lh);
    if (start_sel < 0) return;
    if (start_sel < start_top) start_top = start_sel;
    if (start_top == start_sel && start_sel > 0 && sm_rows[start_sel - 1].item < 0) start_top--;
    for (;;) {
        int yy = 0;
        for (int r = start_top; r <= start_sel; r++) yy += sm_row_height(r);
        if (yy <= lh || start_top >= start_sel) break;
        start_top++;
    }
}

static int sm_row_at(int my) {
    int lx, ly, lw, lh;
    sm_list_box(&lx, &ly, &lw, &lh);
    int yy = ly;
    for (int r = start_top; r < sm_nrows; r++) {
        int rh = sm_row_height(r);
        if (yy + rh > ly + lh) break;
        if (my >= yy && my < yy + rh) return r;
        yy += rh;
    }
    return -1;
}

static void ln_cell(int i, int *cx, int *cy) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    *cx = x + 28 + (i % 6) * LN_CELL_W;
    *cy = y + 116 + (i / 6) * LN_CELL_H;
}

static void ln_recent_row(int i, int *rx, int *ry, int *rw, int *rh) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    *rx = x + 20; *ry = y + 360 + i * 50; *rw = w - 40; *rh = 46;
}

// "All programs" / "Back", at the right of the heading.
static void ln_all_link(int *ax, int *ay, int *aw, int *ah) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    *aw = 150; *ah = 26;
    *ax = x + w - 24 - *aw;
    *ay = y + 84;
}

static void ln_foot_btn(int i, int *bx, int *by) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    *bx = x + w - 100 + i * 44;
    *by = y + h - 52;
}

#define POWER_ROWS 2
static void sm_power_rect(int i, int *px, int *py, int *pw, int *ph) {
    int bx, by;
    ln_foot_btn(1, &bx, &by);
    *pw = 200; *ph = 40;
    *px = bx + 36 - *pw;
    *py = by - 10 - (POWER_ROWS - i) * (*ph);
}

#define KEY_SM(k, a) (0x30000 | ((k) << 10) | (a))
#define SMK_ROW    1
#define SMK_PIN    2
#define SMK_REC    3
#define SMK_ALL    4
#define SMK_FOOT   5
#define SMK_POWER  6
#define SMK_INSIDE 7

static int sm_key_at(int mx, int my) {
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    if (sm_power_open) {
        for (int i = 0; i < POWER_ROWS; i++) {
            int px, py, pw, ph;
            sm_power_rect(i, &px, &py, &pw, &ph);
            if (mx >= px && mx < px + pw && my >= py && my < py + ph) return KEY_SM(SMK_POWER, i);
        }
    }
    if (mx < x || my < y || mx >= x + w || my >= y + h) return -1;
    for (int i = 0; i < 2; i++) {
        int bx, by;
        ln_foot_btn(i, &bx, &by);
        if (mx >= bx && mx < bx + 36 && my >= by && my < by + 36) return KEY_SM(SMK_FOOT, i);
    }
    if (!start_qlen) {
        int ax, ay, aw, ah;
        ln_all_link(&ax, &ay, &aw, &ah);
        if (mx >= ax && mx < ax + aw && my >= ay && my < ay + ah) return KEY_SM(SMK_ALL, 0);
    }
    if (start_qlen || sm_all) {
        int lx, ly, lw, lh;
        sm_list_box(&lx, &ly, &lw, &lh);
        if (mx >= lx && mx < lx + lw && my >= ly && my < ly + lh) {
            int r = sm_row_at(my);
            if (r >= 0 && sm_rows[r].item >= 0) return KEY_SM(SMK_ROW, r);
        }
    } else {
        for (int i = 0; i < 12; i++) {
            int cx, cy;
            ln_cell(i, &cx, &cy);
            if (mx >= cx - 6 && mx < cx + 86 && my >= cy - 6 && my < cy + 86) return KEY_SM(SMK_PIN, i);
        }
        for (int i = 0; i < recent_n && i < 3; i++) {
            int rx, ry, rw, rh;
            ln_recent_row(i, &rx, &ry, &rw, &rh);
            if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) return KEY_SM(SMK_REC, i);
        }
    }
    return KEY_SM(SMK_INSIDE, 0);
}

// --- drawing ---

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' && q[1]) b = q + 1;
    return b;
}

// A program's icon at size s: the drawn one if there is one, else its glyph.
static void prog_icon(const char *file, int x, int y, int s) {
    if (s >= ICON_BIG && ui_icon(x + (s - ICON_BIG) / 2, y + (s - ICON_BIG) / 2, file, ICON_BIG)) return;
    int g;
    uint32_t col;
    glyph_for_program(file, &g, &col);
    glyph_app(x, y, s, col, g);
}

static ukeep pop_keep;
static ui_glass pop_glass;
static int pop_glass_fresh;                 // its memory matches what is under

// What the launcher can reach, wherever the spring has it.
static void sm_reach(int *rx, int *ry, int *rw, int *rh) {
    int x, y, w, h;
    geom_rest = 1;
    start_geom(&x, &y, &w, &h);
    geom_rest = 0;
    int m = 40 + POP_TRAVEL;
    *rx = x - m; *ry = y - m; *rw = w + 2 * m; *rh = h + 2 * m;
}

static void draw_start_menu(void) {
    if (!start_open) return;
    const theme_t *T = TH;
    int x, y, w, h;
    start_geom(&x, &y, &w, &h);
    uint32_t ink = T->title_text, dim = T->title_text_dim;

    ui_shadow(x, y, w, h, 24, 34, 80, 14, 0);
    ui_shadow(x, y, w, h, 24, 4, 34, 1, 0);
    ui_glass_place(&pop_glass, x, y, w, h, 16);
    if (!pop_glass_fresh) {
        ui_glass_forget(&pop_glass);
        ui_glass_take(&pop_glass, 0, 0);
        pop_glass_fresh = 1;
    }
    ui_glass_draw(&pop_glass, x, y, w, h, 24, 0, 0, 0, 0, 0, 0,
                  ui_mix(T->frost, T->accent, 40), 165 + T->frost_a);
    ui_glass_rim(x, y, w, h, 24);

    // Search.
    ui_round_fill(x + 24, y + 24, w - 48, 44, 22, T->plate, 235);
    glyph_draw(G_SEARCH, x + 40, y + 37, 18, 0xFF000000 | dim, 0);
    {
        int ty = y + 46 - ui_line_h(UI_F15) / 2;
        if (start_qlen) {
            int tw = ui_text(x + 70, ty, start_q, UI_F15, ink);
            ui_fill(x + 71 + tw, y + 34, 2, 24, T->accent);         // the caret
        } else {
            ui_text(x + 70, ty, L("Программы и настройки…", "Programs and settings…"), UI_F15, dim);
        }
    }

    int saved[4];
    if (start_qlen || sm_all) {
        // A list: everything, by letter, or what the search found.
        if (!start_qlen) ui_text(x + 28, y + 88, L("Все программы", "All programs"), UI_F13B, ink);
        int lx, ly, lw, lh;
        sm_list_box(&lx, &ly, &lw, &lh);
        ui_clip_get(saved);
        ui_clip(lx, ly, lw, lh);
        int yy = ly;
        for (int r = start_top; r < sm_nrows; r++) {
            int rh = sm_row_height(r);
            if (yy >= ly + lh) break;
            if (sm_rows[r].item < 0) {
                // The initial of the program under it, a whole character.
                const char *nm = start_items[(unsigned char)sm_rows[r].letter].name;
                char lt[5];
                int n = 1;
                unsigned char c0 = (unsigned char)nm[0];
                if (c0 >= 0xC0) n = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : 2;
                for (int k = 0; k < n; k++) lt[k] = nm[k];
                lt[n] = 0;
                if (lt[0] >= 'a' && lt[0] <= 'z') lt[0] = (char)(lt[0] - 32);
                ui_text(lx + 14, yy + (rh - ui_line_h(UI_F13B)) / 2 + 2, lt, UI_F13B,
                        accent_ink(T, 40));
            } else {
                const struct start_item *it = &start_items[sm_rows[r].item];
                int glow = glow_of(KEY_SM(SMK_ROW, r));
                int sel = (r == start_sel);
                if (sel || glow) {
                    int a = (sel ? 150 : 0) + glow * 90 / 256;
                    ui_round_fill(lx + 4, yy + 1, lw - 8, rh - 2, 12, T->plate, a > 230 ? 230 : a);
                }
                prog_icon(it->name_file, lx + 12, yy + (rh - 30) / 2, 30);
                ui_text_fit(lx + 54, yy + (rh - ui_line_h(UI_F13)) / 2, lw - 70, it->name, UI_F13, ink);
            }
            yy += rh;
        }
        if (!sm_nrows) ui_text(lx + 14, ly + 10, L("Ничего не найдено", "Nothing found"), UI_F13, dim);
        ui_clip_set(saved);
    } else {
        // Home: the kept programs, then what was opened lately.
        ui_text(x + 28, y + 88, L("Закреплённые", "Pinned"), UI_F13B, ink);
        for (int i = 0; i < 12; i++) {
            int cx, cy;
            ln_cell(i, &cx, &cy);
            int glow = glow_of(KEY_SM(SMK_PIN, i));
            if (glow) ui_round_fill(cx - 6, cy - 6, 92, 92, 18, T->plate, glow * 170 / 256);
            int pressed = press_key == KEY_SM(SMK_PIN, i) && glow;
            prog_icon(ln_pins[i], cx + 18, cy + 4 + (pressed ? 1 : 0), 44);
            const char *lbl = program_label(ln_pins[i]);
            int tw = ui_text_w(lbl, UI_F12);
            if (tw > LN_CELL_W - 6) {
                ui_clip_get(saved);
                ui_clip(cx - 6, cy + 50, 92, 24);
                ui_text_fit(cx - 3, cy + 56, 86, lbl, UI_F12, ink);
                ui_clip_set(saved);
            } else {
                ui_text(cx + 40 - tw / 2, cy + 56, lbl, UI_F12, ink);
            }
        }
        ui_text(x + 28, y + 330, L("Недавнее", "Recent"), UI_F13B, ink);
        if (!recent_n)
            ui_text(x + 28, y + 364, L("Здесь появится то, что вы открывали.",
                                        "What you open will show up here."), UI_F13, dim);
        for (int i = 0; i < recent_n && i < 3; i++) {
            int rx, ry, rw, rh;
            ln_recent_row(i, &rx, &ry, &rw, &rh);
            int glow = glow_of(KEY_SM(SMK_REC, i));
            if (glow) ui_round_fill(rx, ry, rw, rh, 12, T->plate, glow * 150 / 256);
            const char *file = base_name(recents[i].path);
            prog_icon(file, rx + 12, ry + 7, 32);
            const char *title = recents[i].arg[0] ? base_name(recents[i].arg) : program_label(file);
            char meta[64];
            int q = 0;
            const char *pl = program_label(file);
            while (*pl && q < 40) meta[q++] = *pl++;
            meta[q++] = ' '; meta[q++] = '\xC2'; meta[q++] = '\xB7'; meta[q++] = ' ';   // " · "
            two_digits(meta + q, recents[i].hh); q += 2;
            meta[q++] = ':';
            two_digits(meta + q, recents[i].mm); q += 2;
            meta[q] = 0;
            ui_text_fit(rx + 58, ry + 5, rw - 70, title, UI_F13, ink);
            ui_text(rx + 58, ry + 25, meta, UI_F11, dim);
        }
    }
    if (!start_qlen) {
        int ax, ay, aw, ah;
        ln_all_link(&ax, &ay, &aw, &ah);
        const char *lbl = sm_all ? L("Назад", "Back") : L("Все программы", "All programs");
        int glow = glow_of(KEY_SM(SMK_ALL, 0));
        if (glow) ui_round_fill(ax, ay, aw, ah, 13, T->plate, glow * 160 / 256);
        int tw = ui_text_w(lbl, UI_F12);
        ui_text(ax + aw - 12 - tw, ay + (ah - ui_line_h(UI_F12)) / 2, lbl, UI_F12,
                accent_ink(T, 50));
    }

    // Foot: you, the settings, the power.
    ui_blend(x + 20, y + h - 64, w - 40, 1, T->dark ? 0x00FFFFFF : 0x00000000, 22);
    {
        rast_path p;
        rast_path_init(&p);
        rast_ellipse(&p, RAST_INT(x + 44), RAST_INT(y + h - 34), RAST_INT(16), RAST_INT(16));
        g_fill(&p, 0xFF000000 | T->accent);
        rast_path_free(&p);
        const char *ini = L("П", "U");
        ui_text(x + 44 - ui_text_w(ini, UI_F13B) / 2, y + h - 34 - ui_line_h(UI_F13B) / 2, ini,
                UI_F13B, 0x00FFFFFF);
        ui_text(x + 70, y + h - 34 - ui_line_h(UI_F13) / 2, L("Пользователь", "User"), UI_F13, ink);
    }
    for (int i = 0; i < 2; i++) {
        int bx, by;
        ln_foot_btn(i, &bx, &by);
        int glow = glow_of(KEY_SM(SMK_FOOT, i));
        int on = (i == 1 && sm_power_open);
        ui_round_fill(bx, by, 36, 36, 18, on ? T->accent : T->plate, on ? 255 : 150 + glow * 90 / 256);
        glyph_draw(i ? G_POWER : G_GEAR, bx + 8, by + 8, 20, 0xFF000000 | (on ? 0x00FFFFFF : ink),
                   0xFF000000 | (on ? T->accent : T->plate));
    }

    // The power choice, over everything.
    if (sm_power_open) {
        int px, py, pw, ph;
        sm_power_rect(0, &px, &py, &pw, &ph);
        int bh = POWER_ROWS * ph + 12, by0 = py - 6;
        ui_shadow(px, by0, pw, bh, 16, 16, 70, 5, 0);
        ui_round_fill(px, by0, pw, bh, 16, T->plate, 245);
        const char *const pw_names[POWER_ROWS] = { L("Перезагрузить", "Restart"), L("Выключить", "Shut down") };
        for (int i = 0; i < POWER_ROWS; i++) {
            int rx, ry, rw, rh;
            sm_power_rect(i, &rx, &ry, &rw, &rh);
            int glow = glow_of(KEY_SM(SMK_POWER, i));
            if (glow) ui_round_fill(rx + 4, ry + 1, rw - 8, rh - 2, 12, T->accent, glow * 60 / 256);
            ui_text(rx + 18, ry + (rh - ui_line_h(UI_F13)) / 2, pw_names[i], UI_F13, ink);
        }
    }
}


// --- the control panel ------------------------------------------------------------
//
// It rises from the island's clock: switches for the network, quiet, the
// dark theme and keeping this layout of windows; the volume; what is
// playing; this month. Glass, like the launcher, and the two never show at
// once -- they share the keeping of what is under them.

static void desk_save(void);
static void island_set_edge(int e);

#define CC_W 380
#define CC_H 640
#define CC_TOGGLES 6        // three rows of two
static int dnd;                     // "do not disturb": notices are not shown
static int cc_vol_drag;             // the volume knob is held

static int popup_open(void) { return start_open || cc_open || am_open; }

// A popup was opened or closed: what the next frame has to do about it.
static void wp_dirty_on_popup(void) { wp_dirty = 1; dirty = 1; }

static void cc_geom(int *x, int *y, int *w, int *h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    tb_layout();
    const struct tb_item *s = &tb_items[tb_n - 1];       // the status pill is last
    *w = CC_W;
    *h = CC_H;
    if (*h > H - 2 * TASKBAR_H) *h = H - 2 * TASKBAR_H;
    switch (island_edge) {
    case ISL_LEFT:  *x = isl_x + isl_w + 14; *y = s->y + s->h - *h; break;
    case ISL_RIGHT: *x = isl_x - 14 - *w;    *y = s->y + s->h - *h; break;
    case ISL_TOP:   *x = s->x + s->w - *w;   *y = isl_y + isl_h + 14; break;
    default:        *x = s->x + s->w - *w;   *y = isl_y - 14 - *h; break;
    }
    if (*x < 8) *x = 8;
    if (*x + *w > W - 8) *x = W - 8 - *w;
    if (*y < 8) *y = 8;
    if (*y + *h > H - 8) *y = H - 8 - *h;
    pop_shift(x, y);
}

static void cc_reach(int *rx, int *ry, int *rw, int *rh) {
    int x, y, w, h;
    geom_rest = 1;
    cc_geom(&x, &y, &w, &h);
    geom_rest = 0;
    int m = 40 + POP_TRAVEL;
    *rx = x - m; *ry = y - m; *rw = w + 2 * m; *rh = h + 2 * m;
}

static void cc_toggle_rect(int i, int *tx, int *ty, int *tw, int *th) {
    int x, y, w, h;
    cc_geom(&x, &y, &w, &h);
    *tx = x + 20 + (i % 2) * 174;
    *ty = y + 20 + (i / 2) * 72;
    *tw = 166;
    *th = 62;
}

static void cc_vol_track(int *vx, int *vy, int *vw) {
    int x, y, w, h;
    cc_geom(&x, &y, &w, &h);
    *vx = x + 60; *vy = y + 254 + 10; *vw = w - 84;
}

static void cc_player_rect(int *px, int *py, int *pw, int *ph) {
    int x, y, w, h;
    cc_geom(&x, &y, &w, &h);
    *px = x + 20; *py = y + 302; *pw = w - 40; *ph = 86;
}

#define KEY_CC(k, a) (0x40000 | ((k) << 8) | (a))
#define CCK_TOGGLE 1
#define CCK_VOL    2
#define CCK_PLAYER 3
#define CCK_INSIDE 4

static int cc_key_at(int mx, int my) {
    int x, y, w, h;
    cc_geom(&x, &y, &w, &h);
    if (mx < x || my < y || mx >= x + w || my >= y + h) return -1;
    for (int i = 0; i < CC_TOGGLES; i++) {
        int tx, ty, tw, th;
        cc_toggle_rect(i, &tx, &ty, &tw, &th);
        if (mx >= tx && mx < tx + tw && my >= ty && my < ty + th) return KEY_CC(CCK_TOGGLE, i);
    }
    int vx, vy, vw;
    cc_vol_track(&vx, &vy, &vw);
    if (mx >= vx - 12 && mx < vx + vw + 12 && my >= vy - 14 && my < vy + 26) return KEY_CC(CCK_VOL, 0);
    int px, py, pw, ph;
    cc_player_rect(&px, &py, &pw, &ph);
    if (mx >= px && mx < px + pw && my >= py && my < py + ph) return KEY_CC(CCK_PLAYER, 0);
    return KEY_CC(CCK_INSIDE, 0);
}

static void cc_set_volume_at(int mx) {
    int vx, vy, vw;
    cc_vol_track(&vx, &vy, &vw);
    int v = (mx - vx) * 100 / (vw > 0 ? vw : 1);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume(v);
    menu_dirty = 1;
    dirty = 1;
    tb_valid = 0;                    // the island's speaker shows it too
}

static const char *const month_names[2][12] = {
    { "Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
      "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь" },
    { "January", "February", "March", "April", "May", "June",
      "July", "August", "September", "October", "November", "December" } };

static int days_in(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[(m + 11) % 12];
}

static void draw_control(void) {
    if (!cc_open) return;
    const theme_t *T = TH;
    int x, y, w, h;
    cc_geom(&x, &y, &w, &h);
    uint32_t ink = T->title_text, dim = T->title_text_dim, acc = T->accent;

    ui_shadow(x, y, w, h, 24, 34, 80, 14, 0);
    ui_shadow(x, y, w, h, 24, 4, 34, 1, 0);
    ui_glass_place(&pop_glass, x, y, w, h, 16);
    if (!pop_glass_fresh) {
        ui_glass_forget(&pop_glass);
        ui_glass_take(&pop_glass, 0, 0);
        pop_glass_fresh = 1;
    }
    ui_glass_draw(&pop_glass, x, y, w, h, 24, 0, 0, 0, 0, 0, 0, ui_mix(T->frost, acc, 40), 165 + T->frost_a);
    ui_glass_rim(x, y, w, h, 24);

    // The switches.
    const char *names[CC_TOGGLES] = { L("Сеть", "Network"), L("Не беспокоить", "Do not disturb"),
                                      L("Тёмная тема", "Dark theme"), L("Язык", "Language"),
                                      L("Остров", "Island"), L("Раскладка", "Layout") };
    static const int glyphs[CC_TOGGLES] = { G_WIFI, G_BELL, G_MOON, G_GLOBE, G_MAX, G_SAVE };
    int on[CC_TOGGLES] = { net_is_up(), dnd, TH->dark, 0, 0, 0 };
    const char *edges[4] = { L("Снизу", "Bottom"), L("Слева", "Left"), L("Справа", "Right"), L("Сверху", "Top") };
    const char *sub[CC_TOGGLES] = { on[0] ? L("Подключено", "Connected") : L("Нет сети", "Offline"),
                                    dnd ? L("Вкл.", "On") : L("Выкл.", "Off"),
                                    TH->dark ? L("Вкл.", "On") : L("Выкл.", "Off"),
                                    L("Русский", "English"), edges[island_edge & 3],
                                    L("Сохранить стол", "Save desktop") };
    for (int i = 0; i < CC_TOGGLES; i++) {
        int tx, ty, tw, th;
        cc_toggle_rect(i, &tx, &ty, &tw, &th);
        int glow = glow_of(KEY_CC(CCK_TOGGLE, i));
        uint32_t bg = on[i] ? acc : T->plate;
        ui_round_fill(tx, ty, tw, th, 18, on[i] ? ui_mix(acc, 0xFFFFFF, glow * 40 / 256) : bg,
                      on[i] ? 255 : 170 + glow * 70 / 256);
        uint32_t chip = on[i] ? ui_mix(acc, 0x00FFFFFF, 70) : ui_mix(T->plate, acc, 40);
        ui_round_fill(tx + 10, ty + 13, 36, 36, 18, chip, 255);
        glyph_draw(glyphs[i], tx + 18, ty + 21, 20, on[i] ? 0xFFFFFFFF : (0xFF000000 | accent_ink(T, 60)),
                   0xFF000000 | chip);
        ui_text(tx + 54, ty + 12, names[i], UI_F13B, on[i] ? 0x00FFFFFF : ink);
        ui_text(tx + 54, ty + 32, sub[i], UI_F11, on[i] ? 0x00EEF4FF : dim);
    }

    // The volume.
    {
        int vx, vy, vw;
        cc_vol_track(&vx, &vy, &vw);
        int snd = audio_ready() > 0, v = audio_volume();
        glyph_draw(G_VOL, x + 26, vy - 4, 20, 0xFF000000 | (snd ? ink : dim), 0);
        ui_round_fill(vx, vy, vw, 12, 6, T->plate, 170);
        ui_round_fill(vx, vy, vw * v / 100 + 6, 12, 6, acc, 255);
        int kx = vx + vw * v / 100 - 11;
        int glow = glow_of(KEY_CC(CCK_VOL, 0));
        ui_shadow(kx, vy - 5, 22, 22, 11, 6, 50, 2, 0);
        ui_round_fill(kx, vy - 5, 22, 22, 11, 0x00FFFFFF, 255);
        if (glow || cc_vol_drag) ui_round_fill(kx + 6, vy + 1, 10, 10, 5, acc, cc_vol_drag ? 255 : glow);
    }

    // What is playing.
    {
        int px, py, pw, ph;
        cc_player_rect(&px, &py, &pw, &ph);
        int glow = glow_of(KEY_CC(CCK_PLAYER, 0));
        ui_round_fill(px, py, pw, ph, 18, T->plate, 170 + glow * 60 / 256);
        ui_round_grad(px + 12, py + 12, 62, 62, 14, UI_ALL, 0x00F4A38C, 255, 0x00C0508F, 255);
        glyph_draw(G_MUSIC, px + 29, py + 29, 28, 0xE0FFFFFF, 0);
        const char *t = audio_title();
        ui_text_fit(px + 88, py + 18, pw - 100, t[0] ? t : L("Ничего не играет", "Nothing playing"), UI_F15B, ink);
        ui_text(px + 88, py + 42, t[0] ? L("Музыка · играет", "Music · playing") : L("Музыка", "Music"),
                UI_F12, dim);
    }

    // The month.
    {
        int cy = y + 404, ch = h - 424;
        ui_round_fill(x + 20, cy, w - 40, ch, 18, T->plate, 170);
        char title[32];
        int q = 0;
        const char *mn = month_names[lang][(wm_clock.mon + 11) % 12];
        while (*mn) title[q++] = *mn++;
        title[q++] = ' ';
        q += u2s(title + q, (unsigned)wm_clock.year);
        title[q] = 0;
        ui_text(x + 36, cy + 14, title, UI_F15B, ink);
        static const char *const wds[2][7] = { { "Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс" },
                                               { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" } };
        const char *const *wd = wds[lang];
        for (int i = 0; i < 7; i++) {
            int tw = ui_text_w(wd[i], UI_F11);
            ui_text(x + 36 + i * 44 + 14 - tw / 2, cy + 46, wd[i], UI_F11, dim);
        }
        int first = (dow(wm_clock.year, wm_clock.mon, 1) + 6) % 7;   // Monday = 0
        int nd = days_in(wm_clock.year, wm_clock.mon);
        for (int d = 1; d <= nd; d++) {
            int cell = first + d - 1, col = cell % 7, row = cell / 7;
            int dx = x + 36 + col * 44 + 14, dy = cy + 72 + row * 28;
            if (dy > cy + ch - 24) break;
            char b[3];
            int bl = u2s(b, (unsigned)d);
            b[bl] = 0;
            if (d == wm_clock.day) ui_round_fill(dx - 14, dy - 4, 28, 26, 13, acc, 255);
            int tw = ui_text_w(b, UI_F12);
            ui_text(dx - tw / 2, dy, b, UI_F12, d == wm_clock.day ? 0x00FFFFFF : (col >= 5 ? dim : ink));
        }
    }
}

// --- notices ------------------------------------------------------------------------
//
// A card of glass in the corner -- what is playing now, a layout kept, a
// program's word -- that comes in, stays a few seconds and goes. One at a
// time; a newer one waits its turn.

// --- a program's windows, from the island ------------------------------------------
//
// A right click on a program on the island lays its windows out as small
// pictures of themselves: pick one to bring it forward, close one by its
// cross, close them all -- or open another window of the program instead of
// going back to the one already running.
#define AM_CARD_W  220
#define AM_CARD_H  164
#define AM_THUMB_H 112
#define AM_COLS    4
#define AM_GAP     12
#define AM_MAXW    16
#define AM_TOP     64                  // the header: the program's icon and name
#define AM_FOOT    60                  // the buttons along the bottom
static char am_name[24];               // the program, by its file in /bin
static int  am_win[AM_MAXW], am_n;     // its windows on this desktop

#define AMK_CARD   1
#define AMK_CLOSE  2
#define AMK_NEW    3
#define AMK_ALL    4
#define AMK_INSIDE 5
#define KEY_AM(k, a) (0x60000 | ((k) << 8) | (a))

static void am_collect(void) {
    am_n = 0;
    for (int i = 0; i < MAX_NODES && am_n < AM_MAXW; i++)
        if (nodes[i].used && nodes[i].ws == cur_ws &&
            str_same(pane_title(&panes[nodes[i].pane_idx]), am_name)) am_win[am_n++] = i;
}

static void am_geom(int *x, int *y, int *w, int *h) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    am_collect();
    int cols = am_n < AM_COLS ? am_n : AM_COLS;
    int rows = (am_n + AM_COLS - 1) / AM_COLS;
    *w = cols ? cols * (AM_CARD_W + AM_GAP) - AM_GAP + 40 : 0;
    if (*w < 340) *w = 340;
    *h = AM_TOP + (rows ? rows * (AM_CARD_H + AM_GAP) : 8) + AM_FOOT;
    // By the program's place on the island.
    tb_layout();
    int ax = isl_x + isl_w / 2, ay = isl_y + isl_h / 2;
    for (int i = 0; i < tb_n; i++)
        if (tb_items[i].kind == TB_APP && str_same(tb_items[i].name, am_name)) {
            ax = tb_items[i].x + tb_items[i].w / 2;
            ay = tb_items[i].y + tb_items[i].h / 2;
        }
    switch (island_edge) {
    case ISL_LEFT:  *x = isl_x + isl_w + 14; *y = ay - *h / 2; break;
    case ISL_RIGHT: *x = isl_x - 14 - *w;    *y = ay - *h / 2; break;
    case ISL_TOP:   *x = ax - *w / 2;        *y = isl_y + isl_h + 14; break;
    default:        *x = ax - *w / 2;        *y = isl_y - 14 - *h; break;
    }
    if (*x < 8) *x = 8;
    if (*x + *w > W - 8) *x = W - 8 - *w;
    if (*y < 8) *y = 8;
    if (*y + *h > H - 8) *y = H - 8 - *h;
    pop_shift(x, y);
}

static void am_reach(int *rx, int *ry, int *rw, int *rh) {
    int x, y, w, h;
    geom_rest = 1;
    am_geom(&x, &y, &w, &h);
    geom_rest = 0;
    int m = 40 + POP_TRAVEL;
    *rx = x - m; *ry = y - m; *rw = w + 2 * m; *rh = h + 2 * m;
}

static void am_card_rect(int i, int x, int y, int *cx, int *cy) {
    *cx = x + 20 + (i % AM_COLS) * (AM_CARD_W + AM_GAP);
    *cy = y + AM_TOP + (i / AM_COLS) * (AM_CARD_H + AM_GAP);
}

static void am_close_rect(int cx, int cy, int *bx, int *by) {
    *bx = cx + AM_CARD_W - 36;
    *by = cy + AM_THUMB_H + 18;
}

// The buttons along the bottom: "new window" on the left, "close all" on
// the right (only for more than one).
static void am_btn_rect(int which, int x, int y, int w, int h, int *bx, int *by, int *bw, int *bh) {
    const char *t = which ? L("Закрыть все", "Close all") : L("Новое окно", "New window");
    *bw = ui_text_w(t, UI_F13B) + (which ? 36 : 58);
    *bh = 38;
    *by = y + h - AM_FOOT + 6;
    *bx = which ? x + w - 20 - *bw : x + 20;
}

static int am_key_at(int mx, int my) {
    int x, y, w, h;
    am_geom(&x, &y, &w, &h);
    if (mx < x || my < y || mx >= x + w || my >= y + h) return -1;
    for (int i = 0; i < am_n; i++) {
        int cx, cy, bx, by;
        am_card_rect(i, x, y, &cx, &cy);
        am_close_rect(cx, cy, &bx, &by);
        if (mx >= bx && mx < bx + 26 && my >= by && my < by + 26) return KEY_AM(AMK_CLOSE, i);
        if (mx >= cx && mx < cx + AM_CARD_W && my >= cy && my < cy + AM_CARD_H) return KEY_AM(AMK_CARD, i);
    }
    for (int b = 0; b < (am_n > 1 ? 2 : 1); b++) {
        int bx, by, bw, bh;
        am_btn_rect(b, x, y, w, h, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) return KEY_AM(b ? AMK_ALL : AMK_NEW, 0);
    }
    return KEY_AM(AMK_INSIDE, 0);
}

// A window, small: the program's own picture scaled down, or a terminal's
// text drawn as strokes of its colours -- enough to tell one from another.
static void am_thumb(int n, int x, int y, int w, int h) {
    const struct wm_node *nd = &nodes[n];
    struct pane *p = &panes[nd->pane_idx];
    int ww = (int)nd->w > 0 ? (int)nd->w : 1, wh = (int)nd->h > 0 ? (int)nd->h : 1;
    int tw = w, th = wh * w / ww;
    if (th > h) { th = h; tw = ww * h / wh; }
    if (tw < 8 || th < 8) return;
    int tx = x + (w - tw) / 2, ty = y + (h - th) / 2;
    ui_round_fill(tx, ty, tw, th, 8, body_colour(n), 255);
    if (p->gfx_on && p->gfx && p->gfx_w && p->gfx_h) {
        int W = (int)fb_get_width(), H = (int)fb_get_height();
        volatile uint8_t *base = fb_get_base();
        uint32_t pitch = fb_get_pitch();
        const int r = 8;
        for (int j = 0; j < th; j++) {
            int py = ty + j;
            if (py < 0 || py >= H) continue;
            uint32_t *d = (uint32_t *)(base + (size_t)py * pitch);
            const uint32_t *srow = p->gfx + (size_t)(j * p->gfx_h / th) * p->gfx_w;
            for (int i = 0; i < tw; i++) {
                int px = tx + i;
                if (px < 0 || px >= W) continue;
                // Keep the rounded corners round.
                int cx = i < r ? r - i : i >= tw - r ? i - (tw - r - 1) : 0;
                int cy = j < r ? r - j : j >= th - r ? j - (th - r - 1) : 0;
                if (cx && cy && cx * cx + cy * cy > r * r) continue;
                d[px] = srow[i * p->gfx_w / tw];
            }
        }
        fb_mark_rect((uint32_t)(tx < 0 ? 0 : tx), (uint32_t)(ty < 0 ? 0 : ty), (uint32_t)tw, (uint32_t)th);
        return;
    }
    if (!p->cols || !p->rows) return;
    int pad = 4;
    int iw = tw - 2 * pad, ih = th - 2 * pad;
    int cw = iw * 256 / (int)p->cols, ch = ih * 256 / (int)p->rows;
    int sh = ch * 45 / 100 >> 8;
    if (sh < 1) sh = 1;
    for (uint32_t r = 0; r < p->rows; r++) {
        int ry = ty + pad + (int)((r * ch) >> 8) + (ch >> 10);
        uint32_t c = 0;
        while (c < p->cols) {
            int cp;
            uint8_t at;
            pane_cell(p, r, c, &cp, &at);
            if (cp <= ' ') { c++; continue; }
            // A run of characters in one colour is one stroke.
            uint32_t c0 = c;
            uint8_t fg = (uint8_t)(at >> 4);
            for (;;) {
                c++;
                if (c >= p->cols) break;
                int cp2;
                uint8_t at2;
                pane_cell(p, r, c, &cp2, &at2);
                if (cp2 <= ' ' || (uint8_t)(at2 >> 4) != fg) break;
            }
            int sx = tx + pad + (int)((c0 * cw) >> 8);
            int ex = tx + pad + (int)((c * cw) >> 8);
            ui_fill(sx, ry, ex - sx > 1 ? ex - sx - 1 : 1, sh, pane_palette[fg & 7]);
        }
    }
}

static const char *ru_windows(int n) {
    int a = n % 100, b = n % 10;
    if (a >= 11 && a <= 14) return "окон";
    if (b == 1) return "окно";
    if (b >= 2 && b <= 4) return "окна";
    return "окон";
}

static void draw_appmenu(void) {
    if (!am_open) return;
    const theme_t *T = TH;
    int x, y, w, h;
    am_geom(&x, &y, &w, &h);
    uint32_t ink = T->title_text, dim = T->title_text_dim;

    ui_shadow(x, y, w, h, 24, 34, 80, 14, 0);
    ui_shadow(x, y, w, h, 24, 4, 34, 1, 0);
    ui_glass_place(&pop_glass, x, y, w, h, 16);
    if (!pop_glass_fresh) {
        ui_glass_forget(&pop_glass);
        ui_glass_take(&pop_glass, 0, 0);
        pop_glass_fresh = 1;
    }
    ui_glass_draw(&pop_glass, x, y, w, h, 24, 0, 0, 0, 0, 0, 0, ui_mix(T->frost, T->accent, 40), 165 + T->frost_a);
    ui_glass_rim(x, y, w, h, 24);

    // Whose windows, and how many.
    prog_icon(am_name, x + 20, y + 16, 34);
    ui_text_fit(x + 66, y + 14, w - 90, program_label(am_name), UI_F15B, ink);
    {
        char t[40];
        int q = 0;
        if (!am_n) {
            const char *z = L("Не запущена", "Not running");
            while (*z) t[q++] = *z++;
        } else {
            q += u2s(t, (unsigned)am_n);
            t[q++] = ' ';
            const char *z = lang ? (am_n == 1 ? "window" : "windows") : ru_windows(am_n);
            while (*z) t[q++] = *z++;
        }
        t[q] = 0;
        ui_text(x + 66, y + 36, t, UI_F12, dim);
    }

    for (int i = 0; i < am_n; i++) {
        int n = am_win[i];
        const struct wm_node *nd = &nodes[n];
        int cx, cy;
        am_card_rect(i, x, y, &cx, &cy);
        int glow = glow_of(KEY_AM(AMK_CARD, i));
        int front = n == focused && nd->state != WIN_MIN && !nd->tab_hidden;
        ui_round_fill(cx, cy, AM_CARD_W, AM_CARD_H, 18, T->plate, 120 + glow * 100 / 256);
        if (front) ui_rrect_line(cx, cy, AM_CARD_W, AM_CARD_H, 18, UI_ALL, T->accent, 255);
        am_thumb(n, cx + 10, cy + 10, AM_CARD_W - 20, AM_THUMB_H);

        // What it is: the file it has open, or which of them it is; and if
        // it is not on the screen, why not.
        char t[64];
        int q = 0;
        if (nd->arg[0]) {
            const char *b = base_name(nd->arg);
            while (*b && q < 40) t[q++] = *b++;
        } else {
            const char *b = L("Окно ", "Window ");
            while (*b) t[q++] = *b++;
            q += u2s(t + q, (unsigned)(i + 1));
        }
        t[q] = 0;
        const char *st = nd->tab_hidden ? L("вкладка", "a tab") :
                         nd->state == WIN_MIN ? L("свёрнуто", "minimised") :
                         front ? L("сейчас открыто", "in front") : L("на столе", "on the desktop");
        ui_text_fit(cx + 14, cy + AM_THUMB_H + 16, AM_CARD_W - 60, t, UI_F13B, ink);
        ui_text_fit(cx + 14, cy + AM_THUMB_H + 34, AM_CARD_W - 60, st, UI_F11, dim);

        int bx, by;
        am_close_rect(cx, cy, &bx, &by);
        int cg = glow_of(KEY_AM(AMK_CLOSE, i));
        int pressed = press_key == KEY_AM(AMK_CLOSE, i) && cg;
        ui_round_fill(bx, by, 26, 26, 13, ui_mix(T->plate, 0x00E5484D, cg), 150 + cg * 105 / 256);
        wglyph(GL_CLOSE, bx + 7, by + 7 + (pressed ? 1 : 0), 12,
               0xFF000000 | ui_mix(T->title_text, 0x00FFFFFF, cg));
    }
    if (!am_n)
        ui_text(x + 20, y + AM_TOP, L("Окон этой программы сейчас нет.", "This program has no windows now."),
                UI_F13, dim);

    for (int b = 0; b < (am_n > 1 ? 2 : 1); b++) {
        int bx, by, bw, bh;
        am_btn_rect(b, x, y, w, h, &bx, &by, &bw, &bh);
        int glow = glow_of(KEY_AM(b ? AMK_ALL : AMK_NEW, 0));
        if (b == 0) {
            ui_round_fill(bx, by, bw, bh, bh / 2, ui_mix(T->accent, 0x00FFFFFF, glow * 40 / 256), 255);
            wglyph(GL_PLUS, bx + 16, by + 13, 12, 0xFFFFFFFF);
            ui_text(bx + 36, by + (bh - ui_line_h(UI_F13B)) / 2, L("Новое окно", "New window"), UI_F13B, 0x00FFFFFF);
        } else {
            ui_round_fill(bx, by, bw, bh, bh / 2, ui_mix(T->plate, 0x00E5484D, glow * 200 / 256),
                          170 + glow * 85 / 256);
            ui_text(bx + 18, by + (bh - ui_line_h(UI_F13B)) / 2, L("Закрыть все", "Close all"), UI_F13B,
                    glow > 128 ? 0x00FFFFFF : ink);
        }
    }
}

static void start_toggle(void);

static void am_show(const char *name) {
    if (am_open && str_same(am_name, name)) {      // again on the same: put it away
        am_open = 0;
        wp_dirty = 1;
        dirty = 1;
        return;
    }
    if (start_open) start_toggle();
    cc_open = 0;
    am_open = 1;
    str_put(am_name, name, sizeof am_name);
    pop_t0 = now_ms();
    pop_glass_fresh = 0;
    wp_dirty = 1;
    dirty = 1;
}

#define NOTE_MAX  4
#define NOTE_SHOW 5000
#define NOTE_FADE 260
static struct { char title[48]; char text[96]; int g; uint32_t col; char open[16]; } notes[NOTE_MAX];
static int note_n;
static uint64_t note_t0;            // when the first one in the queue came up
static int note_drawn;              // a notice was on the screen last frame
static ukeep note_keep;
static ui_glass note_glass;

static void note_push(const char *title, const char *text, int g, uint32_t col);

void wm_notify_glyph(const char *title, const char *text, int g, uint32_t col) {
    if (dnd) return;
    note_push(title, text, g, col);
    sound_play(SND_NOTIFY);
}

// A notice into the queue, without a sound (the caller has its own).
static void note_push(const char *title, const char *text, int g, uint32_t col) {
    if (dnd) return;
    if (note_n >= NOTE_MAX) return;
    str_put(notes[note_n].title, title, sizeof notes[0].title);
    str_put(notes[note_n].text, text ? text : "", sizeof notes[0].text);
    notes[note_n].g = g;
    notes[note_n].col = col;
    notes[note_n].open[0] = 0;
    if (note_n == 0) note_t0 = now_ms();
    note_n++;
    dirty = 1;
}

// The notice just pushed opens this folder when it is clicked.
static void note_open_last(const char *path) {
    if (note_n) str_put(notes[note_n - 1].open, path, sizeof notes[0].open);
}

void wm_notify(const char *title, const char *text) {
    wm_notify_glyph(title, text, G_BELL, 0x003C7FD8);
}

static void note_geom(int *x, int *y, int *w, int *h) {
    *w = 380; *h = 86;
    *x = (int)fb_get_width() - *w - 24;
    *y = island_edge == ISL_TOP ? TASKBAR_H + 10 : 24;
    if (island_edge == ISL_RIGHT) *x -= TASKBAR_H - 10;
}

// How far in the first notice is: 0..256, or -1 when it is done.
static int note_alpha(void) {
    if (!note_n) return -1;
    uint64_t t = now_ms() - note_t0;
    if (t < NOTE_FADE) return (int)(t * 256 / NOTE_FADE);
    if (t < NOTE_SHOW) return 256;
    if (t < NOTE_SHOW + NOTE_FADE) return 256 - (int)((t - NOTE_SHOW) * 256 / NOTE_FADE);
    return -1;
}

#define NOTE_SLIDE 48

static int note_animating(void) {
    if (!note_n) return 0;
    uint64_t t = now_ms() - note_t0;
    return t < SPRING_MS || t >= NOTE_SHOW;
}

static void note_next(void) {
    for (int i = 1; i < note_n; i++) notes[i - 1] = notes[i];
    note_n--;
    note_t0 = now_ms();
}

static void draw_note(void) {
    int x, y, w, h;
    note_geom(&x, &y, &w, &h);
    int rx = x - 40, ry = y - 30, rw = w + 80 + NOTE_SLIDE, rh = h + 80;
    int a = note_alpha();
    if (a < 0 && note_n) { note_next(); a = note_alpha(); }
    if (a < 0 && !note_drawn) return;
    if (!note_animating() && note_drawn && a >= 0 && !dmg_meets(rx, ry, rw, rh)) return;

    uk_place(&note_keep, rx, ry, rw, rh);
    uk_take(&note_keep);
    uk_restore(&note_keep);
    if (a < 0) { note_drawn = 0; dmg_add(rx, ry, rx + rw, ry + rh); return; }

    const theme_t *T = TH;
    {   // Coming in: from a little way out, on the spring.
        uint64_t nt = now_ms() - note_t0;
        if (nt < SPRING_MS) x += (256 - spring_at(nt)) * NOTE_SLIDE / 256;
    }
    ui_shadow(x, y, w, h, 20, 26, 70, 10, 0);
    ui_glass_place(&note_glass, x, y, w, h, 16);
    ui_glass_forget(&note_glass);
    ui_glass_take(&note_glass, 0, 0);
    ui_glass_draw(&note_glass, x, y, w, h, 20, 0, 0, 0, 0, 0, 0, ui_mix(T->frost, T->accent, 40),
                  170 + T->frost_a);
    ui_glass_rim(x, y, w, h, 20);
    glyph_app(x + 18, y + 20, 44, notes[0].col, notes[0].g);
    ui_text_fit(x + 76, y + 18, w - 100, notes[0].title, UI_F13B, T->title_text);
    ui_text_fit(x + 76, y + 40, w - 100, notes[0].text, UI_F13, T->title_text_dim);

    // Fading: the card blended with what it covers.
    if (a < 256) {
        volatile uint8_t *base = fb_get_base();
        uint32_t pitch = fb_get_pitch();
        for (int j = 0; j < note_keep.h; j++) {
            uint32_t *d = (uint32_t *)(base + (size_t)(note_keep.y + j) * pitch) + note_keep.x;
            const uint32_t *k = note_keep.px + (size_t)j * note_keep.w;
            for (int i = 0; i < note_keep.w; i++) d[i] = ui_mix(k[i], d[i], a);
        }
    }
    fb_mark_rect((uint32_t)note_keep.x, (uint32_t)note_keep.y, (uint32_t)note_keep.w, (uint32_t)note_keep.h);
    note_drawn = 1;
    dmg_add(rx, ry, rx + rw, ry + rh);
}

static int note_hit(int mx, int my) {
    if (!note_n) return 0;
    int x, y, w, h;
    note_geom(&x, &y, &w, &h);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// --- keeping the layout of windows ----------------------------------------------------
//
// "Keep this layout": which programs have windows, with what in them, where
// and how big -- written to /home/.layout, and opened again just so when the
// desktop next starts.

#define LAYOUT_FILE "/home/.layout"

static int layout_save(void) {
    static char buf[4096];
    int n = 0;
    int win[MAX_NODES];
    int nw = collect_windows_ex(cur_ws, win, MAX_NODES, 1);
    // In tab order, so a frame's tabs come back in theirs.
    for (int i = 1; i < nw; i++) {
        int v = win[i], j = i - 1;
        while (j >= 0 && nodes[win[j]].tab_seq > nodes[v].tab_seq) { win[j + 1] = win[j]; j--; }
        win[j + 1] = v;
    }
    for (int i = 0; i < nw && n < (int)sizeof buf - 300; i++) {
        const struct wm_node *nd = &nodes[win[i]];
        if (nd->state == WIN_MIN) continue;
        // path|arg|x|y|w|h|maximised|group|in front
        const char *p = nd->path[0] ? nd->path : "-";
        for (int k = 0; p[k]; k++) buf[n++] = p[k];
        buf[n++] = '|';
        for (int k = 0; nd->arg[k] && k < 95; k++) buf[n++] = nd->arg[k];
        buf[n++] = '|';
        uint32_t v[7] = { nd->state == WIN_MAX ? nd->sx : nd->x, nd->state == WIN_MAX ? nd->sy : nd->y,
                          nd->state == WIN_MAX ? nd->sw : nd->w, nd->state == WIN_MAX ? nd->sh : nd->h,
                          nd->state == WIN_MAX, (uint32_t)nd->grp, (uint32_t)!nd->tab_hidden };
        for (int k = 0; k < 7; k++) {
            n += u2s(buf + n, v[k]);
            buf[n++] = k < 6 ? '|' : '\n';
        }
    }
    vfs_unlink(LAYOUT_FILE);
    if (vfs_create(LAYOUT_FILE) != 0) return 0;
    int fd = vfs_open(LAYOUT_FILE);
    if (fd < 0) return 0;
    int ok = vfs_write(fd, buf, (uint32_t)n) == n;
    vfs_close(fd);
    return ok;
}

static int quiet_open;          // open without the window's arrival animation
static int new_window_run(const char *path, const char *arg);
static void toggle_maximize(void);
static void restore_window(int n);

// Returns how many windows it opened.
static int layout_restore(void) {
    static char buf[4096];
    int opened = 0;
    int fd = vfs_open(LAYOUT_FILE);
    if (fd < 0) return 0;
    int n = vfs_read(fd, buf, sizeof buf - 1);
    vfs_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    // Saved group -> the first window opened again for it; and who was in front.
    int sg[16], sn[16], ns = 0, fronts[16], nf = 0;
    quiet_open = 1;
    char *line = buf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char save = *end;
        *end = 0;
        // Split on '|' into seven fields, or nine with the tabs.
        char *f[9];
        int k = 0;
        f[k++] = line;
        for (char *c = line; *c && k < 9; c++) if (*c == '|') { *c = 0; f[k++] = c + 1; }
        if (k >= 7) {
            uint32_t v[7] = { 0, 0, 0, 0, 0, 0, 1 };
            for (int j = 0; j < k - 2; j++) {
                v[j] = 0;
                for (char *c = f[2 + j]; *c >= '0' && *c <= '9'; c++) v[j] = v[j] * 10 + (uint32_t)(*c - '0');
            }
            int pid = new_window_run(f[0][0] == '-' ? 0 : f[0], f[1][0] ? f[1] : 0);
            if (pid > 0) opened++;
            if (pid > 0 && focused >= 0) {
                struct wm_node *nd = &nodes[focused];
                nd->x = v[0]; nd->y = v[1]; nd->w = v[2]; nd->h = v[3];
                if (v[4]) toggle_maximize();
                layout_window(focused);
                if (k == 9) {
                    int me = focused, at = -1;
                    for (int q = 0; q < ns; q++) if (sg[q] == (int)v[5]) at = q;
                    if (at >= 0 && nodes[sn[at]].used) tab_join(me, tab_front_of(nodes[sn[at]].grp));
                    else if (ns < 16) { sg[ns] = (int)v[5]; sn[ns] = me; ns++; }
                    if (v[6] && nf < 16) fronts[nf++] = me;
                }
            }
        }
        *end = save;
        line = *end ? end + 1 : end;
    }
    quiet_open = 0;
    for (int i = 0; i < nf; i++) if (nodes[fronts[i]].used) restore_window(fronts[i]);
    relayout();
    return opened;
}

// --- the desktop's settings ----------------------------------------------------
//
// The language, the theme, the island's edge and "do not disturb", kept in
// /home/.desk as name=value lines and read back when the desktop starts.
#define DESK_FILE "/home/.desk"

static void desk_save(void) {
    char buf[96];
    int n = 0;
    const char *k[4] = { "lang=", "theme=", "island=", "dnd=" };
    int v[4] = { lang, theme, island_edge, dnd };
    for (int i = 0; i < 4; i++) {
        for (const char *c = k[i]; *c; c++) buf[n++] = *c;
        n += u2s(buf + n, (unsigned)v[i]);
        buf[n++] = '\n';
    }
    vfs_unlink(DESK_FILE);
    if (vfs_create(DESK_FILE) != 0) return;
    int fd = vfs_open(DESK_FILE);
    if (fd < 0) return;
    vfs_write(fd, buf, (uint32_t)n);
    vfs_close(fd);
}

static void desk_load(void) {
    char buf[128];
    int fd = vfs_open(DESK_FILE);
    if (fd < 0) return;
    int n = vfs_read(fd, buf, sizeof buf - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; *line; ) {
        char *eq = line;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        int v = 0;
        char *c = *eq == '=' ? eq + 1 : eq;
        while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0');
        if (*eq == '=') {
            *eq = 0;
            if (str_same(line, "lang")) lang = v ? 1 : 0;
            else if (str_same(line, "theme")) wm_theme_set(v);
            else if (str_same(line, "island")) island_edge = v & 3;
            else if (str_same(line, "dnd")) dnd = v ? 1 : 0;
        }
        while (*c && *c != '\n') c++;
        line = *c ? c + 1 : c;
    }
}

// A click inside the open control panel.
static void cc_click(int key) {
    int kind = (key >> 8) & 0xFF, arg = key & 0xFF;
    switch (kind) {
    case CCK_TOGGLE:
        if (arg == 0) {
            cc_open = 0;
            wp_dirty = 1;
            new_window_run("/bin/netlog", 0);
        } else if (arg == 1) {
            dnd = !dnd;
            desk_save();
        } else if (arg == 2) {
            wm_theme_set(theme == 0 ? 1 : 0);
        } else if (arg == 3) {
            lang = !lang;
            start_build();                 // the programs by their names in it
            desk_save();
            wp_dirty = 1;
        } else if (arg == 4) {
            // The island goes round the edges: bottom, left, right, top.
            island_set_edge((island_edge + 1) & 3);
            pop_t0 = now_ms();             // the panel comes up again by it
        } else {
            if (layout_save())
                wm_notify_glyph(L("Раскладка сохранена", "Layout saved"),
                                L("Окна откроются так же при следующем запуске.",
                                  "Windows will open the same way next time."),
                                G_SAVE, TH->accent);
            else
                wm_notify_glyph(L("Не удалось сохранить", "Could not save"),
                                L("Нет места в /home или диск только для чтения.",
                                  "No room in /home, or the disk is read-only."),
                                G_SAVE, 0x00C24A2F);
        }
        menu_dirty = 1;
        dirty = 1;
        tb_valid = 0;
        break;
    case CCK_PLAYER: {
        // To the player's window, or start it.
        int target = -1;
        for (int i = 0; i < MAX_NODES; i++)
            if (nodes[i].used && str_same(pane_title(&panes[nodes[i].pane_idx]), "play")) target = i;
        cc_open = 0;
        wp_dirty = 1;
        if (target >= 0) restore_window(target);
        else new_window_run("/bin/play", 0);
        refresh_leaves();
        dirty = 1;
        break;
    }
    default:
        break;
    }
}

static void draw_alttab(void) {
    if (!alttab_active) return;
    const theme_t *T = TH;
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    if (n <= 1) return;

    // A card of programs, one cell each: its icon and its name, the one
    // that Alt will let go on lit.
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    int cw = 112, chh = 112;
    if (n * cw + 32 > W - 80) cw = (W - 80 - 32) / n;
    int bw = n * cw + 32, bh = chh + 32;
    int bx = (W - bw) / 2, by = (H - bh) / 2;

    ui_shadow(bx, by, bw, bh, 26, 34, 80, 14, 0);
    ui_glass_live(bx, by, bw, bh, 26, ui_mix(T->frost, T->accent, 40), 170 + T->frost_a, 12);
    ui_glass_rim(bx, by, bw, bh, 26);
    for (int i = 0; i < n; i++) {
        int cx = bx + 16 + i * cw, cy = by + 16;
        int sel = win[i] == focused;
        if (sel) {
            ui_round_fill(cx + 4, cy, cw - 8, chh, 18, T->plate, 220);
            ui_rrect_line(cx + 4, cy, cw - 8, chh, 18, UI_ALL, T->accent, 255);
        }
        const char *file = pane_title(&panes[nodes[win[i]].pane_idx]);
        int is = cw >= 80 ? 52 : cw - 28;
        prog_icon(file, cx + (cw - is) / 2, cy + 14, is);
        const char *lbl = program_label(file);
        uint32_t c = nodes[win[i]].state == WIN_MIN ? T->title_text_dim : T->title_text;
        int lw = ui_text_w(lbl, UI_F12);
        if (lw > cw - 16) ui_text_fit(cx + 8, cy + 78, cw - 16, lbl, UI_F12, c);
        else ui_text(cx + (cw - lw) / 2, cy + 78, lbl, UI_F12, c);
    }
}

// The desktop underneath the arrow, saved before it is stamped on. Restoring
// this at the start of the next frame is what keeps a moving pointer from
// smearing a trail -- and it costs a couple of hundred pixels instead of the
// full-screen repaint that treating the cursor as "damage" would force.
static uint32_t cursor_save[CUR_SZ * CUR_SZ];
static int cur_saved = 0, cur_saved_x, cur_saved_y;

// A program on its way up: from the moment it is launched until it puts
// its first picture in its window -- or a few seconds, whichever is first
// -- the pointer carries a spinning ring, as Windows' "working in the
// background" pointer does.
#define BUSY_MS 5000
static uint64_t busy_until;
static int busy_pane = -1;

static int busy_active(void) { return busy_until && now_ms() < busy_until; }

static void busy_start(int pane) {
    busy_pane = pane;
    busy_until = now_ms() + BUSY_MS;
}

static void busy_end(int pane) {
    if (pane == busy_pane || pane < 0) { busy_until = 0; busy_pane = -1; }
}

static int cursor_pick(int mx, int my);      // which picture, by what is under it

static void cursor_restore(void) {
    if (!cur_saved) return;
    volatile uint8_t *base = fb_get_base();
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    uint32_t pitch = fb_get_pitch();
    for (int r = 0; r < CUR_SZ; r++) {
        int py = cur_saved_y + r;
        if (py < 0 || py >= fh) continue;
        uint32_t *drow = (uint32_t *)(base + (size_t)py * pitch);
        for (int c = 0; c < CUR_SZ; c++) {
            int px = cur_saved_x + c;
            if (px < 0 || px >= fw) continue;
            drow[px] = cursor_save[r * CUR_SZ + c];
        }
    }
    int x = cur_saved_x < 0 ? 0 : cur_saved_x, y = cur_saved_y < 0 ? 0 : cur_saved_y;
    fb_mark_rect((uint32_t)x, (uint32_t)y, CUR_SZ, CUR_SZ);
    cur_saved = 0;
}

static void cursor_draw(void) {
    if (!mouse_present()) return;
    int32_t mx, my;
    mouse_position(&mx, &my);

    int hx = 0, hy = 0;
    const uint32_t *img = cursor_image(cursor_pick(mx, my), (int)(now_ms() / 60), &hx, &hy);
    if (!img) return;
    int ox = mx - hx, oy = my - hy;

    volatile uint8_t *base = fb_get_base();
    int fw = (int)fb_get_width(), fh = (int)fb_get_height();
    uint32_t pitch = fb_get_pitch();
    for (int r = 0; r < CUR_SZ; r++) {
        int py = oy + r;
        if (py < 0 || py >= fh) continue;
        uint32_t *drow = (uint32_t *)(base + (size_t)py * pitch);
        for (int c = 0; c < CUR_SZ; c++) {
            int px = ox + c;
            if (px < 0 || px >= fw) continue;
            uint32_t d = drow[px];
            cursor_save[r * CUR_SZ + c] = d;             // remember, then stamp
            uint32_t s = img[r * CUR_SZ + c], a = s >> 24;
            if (!a) continue;
            uint32_t ia = 256 - (a + (a >> 7));
            uint32_t rb = (((d & 0xFF00FF) * ia >> 8) & 0xFF00FF) + (s & 0xFF00FF);
            uint32_t g  = (((d & 0x00FF00) * ia >> 8) & 0x00FF00) + (s & 0x00FF00);
            drow[px] = (rb & 0xFF00FF) | (g & 0xFF00);
        }
    }
    int x = ox < 0 ? 0 : ox, y = oy < 0 ? 0 : oy;
    fb_mark_rect((uint32_t)x, (uint32_t)y, CUR_SZ, CUR_SZ);
    cur_saved = 1;
    cur_saved_x = ox;
    cur_saved_y = oy;
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
    if (started) desk_save();
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

// --- Aero Shake -----------------------------------------------------------------
//
// Grab a window by its title and shake it: every other window is minimised.
// Shake it again and they come back. A shake is four turns of direction,
// each a real movement, inside a second and a half.

#define SHAKE_STEP   18
#define SHAKE_TURNS  4
#define SHAKE_MS     1500
static int shake_dir, shake_turns, shake_x, shake_fired;
static uint64_t shake_t0;
static uint8_t shaken[MAX_NODES];            // minimised by the last shake
static int shake_have;

static void shake_reset(int mx) {
    shake_dir = 0; shake_turns = 0; shake_x = mx; shake_fired = 0;
}

static void aero_shake(int keep) {
    if (shake_have) {                         // the second shake: put them back
        for (int i = 0; i < MAX_NODES; i++)
            if (shaken[i] && nodes[i].used && nodes[i].state == WIN_MIN) nodes[i].state = WIN_NORMAL;
        for (int i = 0; i < MAX_NODES; i++) shaken[i] = 0;
        shake_have = 0;
    } else {
        for (int i = 0; i < MAX_NODES; i++) {
            shaken[i] = 0;
            if (i == keep || !nodes[i].used || nodes[i].ws != cur_ws) continue;
            if (nodes[i].state == WIN_MIN) continue;
            nodes[i].state = WIN_MIN;
            shaken[i] = 1;
            shake_have = 1;
        }
    }
    focused = keep;
    relayout();
    dirty = 1;
}

static void shake_track(int mx, int win) {
    int dx = mx - shake_x;
    if (dx > -SHAKE_STEP && dx < SHAKE_STEP) return;
    int d = dx > 0 ? 1 : -1;
    shake_x = mx;
    if (d == shake_dir) return;
    if (shake_dir) {
        if (!shake_turns || now_ms() - shake_t0 > SHAKE_MS) { shake_turns = 0; shake_t0 = now_ms(); }
        shake_turns++;
    }
    shake_dir = d;
    if (!shake_fired && shake_turns >= SHAKE_TURNS) {
        shake_fired = 1;
        aero_shake(win);
    }
}

// --- windows coming and going -----------------------------------------------------
//
// A window that opens grows into place from a little smaller and fades in; one
// that closes does the reverse; one that is minimised shrinks into its button
// on the taskbar, and comes back out of it.
//
// Done with two pictures of the same piece of screen: without the window (A)
// and with it (B). Every pixel where they differ belongs to the window --
// its shadow included -- and is drawn from B, scaled about the window's
// centre, at the animation's opacity, over A. Short: a program never waits on
// it, and anything that changed meanwhile is put right by a full frame at the
// end.

#define ANIM_MS 170
#define AN_OPEN    1
#define AN_CLOSE   2
#define AN_MIN     3
#define AN_RESTORE 4

static struct {
    int active, kind;
    int x, y, w, h;               // the piece of screen
    int wx, wy, ww, wh;           // the window in it
    int tx, ty;                   // where a minimised window goes to
    uint32_t *a, *b;
    int have_a, have_b;
    int cap;
    uint64_t t0;
    int frames;                   // drawn so far; the clock starts at the first
} anim;

static void render_all(void);

static int anim_buffers(int w, int h) {
    if (anim.a && anim.cap >= w * h) return 1;
    if (anim.a) kfree(anim.a);
    if (anim.b) kfree(anim.b);
    anim.a = (uint32_t *)kmalloc((size_t)w * h * 4);
    anim.b = (uint32_t *)kmalloc((size_t)w * h * 4);
    anim.cap = (anim.a && anim.b) ? w * h : 0;
    return anim.cap > 0;
}

static void grab(uint32_t *dst) {
    px_copy(dst, anim.w, bb_at(anim.x, anim.y), bb_stride(), anim.w, anim.h);
}

// Where window n's button on the taskbar is, for minimising into it.
static void tb_button_center(int n, int *x, int *y) {
    tb_layout();
    *x = isl_x + isl_w / 2;
    *y = isl_y + isl_h / 2;
    const char *t = pane_title(&panes[nodes[n].pane_idx]);
    for (int i = 0; i < tb_n; i++)
        if (tb_items[i].kind == TB_APP && str_same(tb_items[i].name, t)) {
            *x = tb_items[i].x + tb_items[i].w / 2;
            *y = tb_items[i].y + tb_items[i].h / 2;
            return;
        }
}

static void anim_stop(void) {
    if (!anim.active) return;
    anim.active = 0;
    wp_dirty = 1;                      // the true picture, everything current
    dirty = 1;
}

// Set the animation up for window n. Its geometry must be final.
static int anim_setup(int kind, int n) {
    anim_stop();
    if (n < 0 || !nodes[n].used || !wallpaper) return 0;
    int e[4];
    window_extent(n, e);
    const struct wm_node *nd = &nodes[n];
    anim.wx = (int)nd->x; anim.wy = (int)nd->y; anim.ww = (int)nd->w; anim.wh = (int)nd->h;
    anim.tx = anim.wx + anim.ww / 2;
    anim.ty = anim.wy + anim.wh / 2;
    if (kind == AN_MIN || kind == AN_RESTORE) {
        tb_button_center(n, &anim.tx, &anim.ty);
        if (anim.tx - 30 < e[0]) e[0] = anim.tx - 30;
        if (anim.tx + 30 > e[2]) e[2] = anim.tx + 30;
        if (anim.ty - 30 < e[1]) e[1] = anim.ty - 30;
        if (anim.ty + 30 > e[3]) e[3] = anim.ty + 30;
    }
    if (e[0] < 0) e[0] = 0;
    if (e[1] < 0) e[1] = 0;
    if (e[2] > (int)fb_get_width()) e[2] = (int)fb_get_width();
    if (e[3] > (int)fb_get_height()) e[3] = (int)fb_get_height();
    anim.x = e[0]; anim.y = e[1]; anim.w = e[2] - e[0]; anim.h = e[3] - e[1];
    if (anim.w <= 0 || anim.h <= 0 || !anim_buffers(anim.w, anim.h)) return 0;
    anim.kind = kind;
    anim.have_a = anim.have_b = 0;
    anim.frames = 0;
    return 1;
}

// A window that is about to appear (just opened, or coming back from the
// taskbar): picture the screen without it first.
static void anim_appear(int kind, int n) {
    if (!started || !anim_setup(kind, n)) return;
    int st = nodes[n].state;
    nodes[n].state = WIN_MIN;          // not on the screen, for one frame
    wp_dirty = 1;
    render_all();
    grab(anim.a);
    anim.have_a = 1;
    nodes[n].state = st;
    relayout();
    anim.active = 1;
    anim.t0 = now_ms();
    dirty = 1;
}

// A window that is about to go (closed, or minimised): picture it while it
// is still there.
static void anim_vanish(int kind, int n) {
    if (!started || !anim_setup(kind, n)) return;
    cursor_restore();
    outline_hide();
    grab(anim.b);
    anim.have_b = 1;
    anim.active = 1;
    anim.t0 = now_ms();
    dirty = 1;
}

// The parts of a frame of it that every row needs, for the bands.
struct anim_ctx { int64_t inv, fx0; int wcy, cy, alpha; };

static void anim_band(int j0, int j1, int share, void *cv) {
    (void)share;
    const struct anim_ctx *c = (const struct anim_ctx *)cv;
    for (int j = j0; j < j1; j++) {
        uint32_t *d = bb_at(anim.x, anim.y + j);
        const uint32_t *ra = anim.a + j * anim.w;
        int sy = c->wcy + (int)(((int64_t)(j - c->cy) * c->inv) >> 16);
        if (c->alpha <= 0 || sy < 0 || sy >= anim.h) {
            // A row the window does not reach: the picture without it.
            for (int i = 0; i < anim.w; i++) d[i] = ra[i];
            continue;
        }
        const uint32_t *sa = anim.a + sy * anim.w;
        const uint32_t *sb = anim.b + sy * anim.w;
        int64_t fx = c->fx0;
        for (int i = 0; i < anim.w; i++, fx += c->inv) {
            int sx = (int)(fx >> 16);
            uint32_t out = ra[i];
            if ((unsigned)sx < (unsigned)anim.w && sb[sx] != sa[sx])
                out = c->alpha >= 256 ? sb[sx] : ui_mix(out, sb[sx], c->alpha);
            d[i] = out;
        }
    }
}

// One frame of it, over what render_all() has just drawn.
static void anim_frame(void) {
    if (!anim.active) return;
    // The clock starts with the first frame actually drawn: whatever kept the
    // desktop from drawing until now must not eat the start of the motion.
    if (!anim.frames) anim.t0 = now_ms();
    anim.frames++;
    if (!anim.have_a) { grab(anim.a); anim.have_a = 1; }
    if (!anim.have_b) { grab(anim.b); anim.have_b = 1; }

    uint64_t el = now_ms() - anim.t0;
    int arriving = anim.kind == AN_OPEN || anim.kind == AN_RESTORE;
    int dur = arriving ? SPRING_MS : ANIM_MS;
    int t = el >= (uint64_t)dur ? 256 : (int)(el * 256 / (uint64_t)dur);
    int u, alpha;
    if (arriving) {
        u = spring_at(el);                         // a little past full size, and back
        alpha = el >= 120 ? 256 : (int)(el * 256 / 120);
    } else {
        u = 256 - ((t * t) >> 8);                  // away, faster and faster
        alpha = u;
    }
    int small = (anim.kind == AN_MIN || anim.kind == AN_RESTORE) ? 70 : 200;
    int s = small + (256 - small) * u / 256;                   // scale, 256 = 1
    if (s < 8) s = 8;
    int wcx = anim.wx + anim.ww / 2 - anim.x, wcy = anim.wy + anim.wh / 2 - anim.y;
    int cx = anim.tx - anim.x + (wcx - (anim.tx - anim.x)) * u / 256;
    int cy = anim.ty - anim.y + (wcy - (anim.ty - anim.y)) * u / 256;
    int64_t inv = ((int64_t)256 << 16) / s;
    // The source column for the first one, in 16.16; each next is `inv` on.
    int64_t fx0 = ((int64_t)wcx << 16) - (int64_t)cx * inv;

    struct anim_ctx ac = { inv, fx0, wcy, cy, alpha };
    ui_bands(anim_band, &ac, 0, anim.h, anim.w * 2);
    fb_mark_rect((uint32_t)anim.x, (uint32_t)anim.y, (uint32_t)anim.w, (uint32_t)anim.h);
    if (t >= 256) anim_stop();
}

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
//
// The start menu is not on the list: it keeps what it covers (sm_save_under)
// and redraws over that, so only a change UNDER it costs a full frame.
// A popup just come up is still fading in over what it covers.
#define POP_FADE_MS 140
static void pop_fade(void) {
    uint64_t el = now_ms() - pop_t0;
    if (el < POP_FADE_MS) uk_fade(&pop_keep, (int)(el * 256 / POP_FADE_MS));
}

static int pop_animating(void) {
    return popup_open() && now_ms() - pop_t0 < SPRING_MS + 20;
}

static int overlay_needs_repaint(void) {
    return wm_message_pending() || snap_hint != SNAP_NONE || udrag_active ||
           alttab_active;
}

// Whether any visible window would paint differently now.
static int windows_changed(void) {
    int win[MAX_NODES];
    int n = collect_windows(cur_ws, win, MAX_NODES);
    for (int i = 0; i < n; i++)
        if (nodes[win[i]].state != WIN_MIN && pane_changed(win[i])) return 1;
    return 0;
}

static uint32_t *boot_px;       // the boot screen's picture, while it fades
static void boot_fade(void);

static void render_all(void) {
    uint64_t t0 = pf_on ? rdtsc() : 0, t1, t2, t3, t4;
    last_frame_ms = now_ms();
    if (sample_stats() && popup_open()) menu_dirty = 1;   // the clock moved on
    cursor_restore();   // put back what the arrow covered last frame, first
    outline_hide();     // and the drag outline, which sits under the arrow
    // Kernel logging draws straight into the back buffer, and a wallpaper
    // repaint covers the whole screen (panes included), so both force the
    // cached layers to be painted again.
    if (fb_console_wrote()) wp_dirty = 1;
    if (overlay_needs_repaint()) wp_dirty = 1;   // see the note above

    // Only the open menu or panel changed -- a hover, a key, the clock: put
    // back what it covered and draw it again. Nothing under it is touched.
    if (popup_open() && !wp_dirty && pop_keep.have && !monitor_on && !anim.active &&
        !windows_changed()) {
        ndmg = 0;
        dmg_all = 0;
        if (draw_taskbar()) {
            // The island was drawn again, and part of it is under the popup:
            // that part of what the popup keeps is out of date.
            int rx, ry, rw, rh;
            island_reach(&rx, &ry, &rw, &rh);
            uk_copy_in(&pop_keep, rx, ry, rx + rw, ry + rh);
            pop_glass_fresh = 0;
            menu_dirty = 1;
        }
        if (menu_dirty) {
            uk_restore(&pop_keep);
            draw_start_menu();
            draw_control();
            draw_appmenu();
            pop_fade();
            dmg_add(pop_keep.x, pop_keep.y, pop_keep.x + pop_keep.w, pop_keep.y + pop_keep.h);
        }
        menu_dirty = 0;
        draw_note();
        outline_show();
        cursor_draw();
        fb_present();
        return;
    }
    // Anything else under an open popup: rebuild the frame clean, so what the
    // popup saves of it is the desktop and not an old copy of itself.
    if (popup_open()) wp_dirty = 1;
    if (wp_dirty) { invalidate_pane_cache(); tb_valid = 0; }
    int full = wp_dirty;
    ndmg = 0;                 // nothing painted yet this frame
    dmg_all = full;           // ...or everything is about to be
    draw_wallpaper();
    t1 = pf_on ? rdtsc() : 0;
    render_windows();
    t2 = pf_on ? rdtsc() : 0;
    if (monitor_on) draw_gadget();
    draw_snap_preview();
    draw_alttab();
    draw_taskbar();
    if (popup_open()) {
        int rx, ry, rw, rh;
        if (start_open) sm_reach(&rx, &ry, &rw, &rh);
        else if (cc_open) cc_reach(&rx, &ry, &rw, &rh);
        else am_reach(&rx, &ry, &rw, &rh);
        uk_place(&pop_keep, rx, ry, rw, rh);
        uk_take(&pop_keep);
        pop_glass_fresh = 0;
        draw_start_menu();
        draw_control();
        draw_appmenu();
        pop_fade();
        dmg_add(rx, ry, rx + rw, ry + rh);
        menu_dirty = 0;
    } else {
        pop_keep.have = 0;
    }
    draw_note();          // a notice in the corner, over everything but the modal box
    anim_frame();         // a window on its way in or out, over the finished frame
    draw_drag_ghost();    // follows the cursor, above the windows
    draw_message_box();   // above everything: it is modal
    boot_fade();          // the boot screen going, over the first frames
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
        kprintf("gfx: window parts kcyc shadow=%u glass-memory=%u glass=%u frame=%u\n",
                (unsigned)(pf_sh / pf_frames / 1000), (unsigned)(pf_tk / pf_frames / 1000),
                (unsigned)(pf_gl / pf_frames / 1000), (unsigned)(pf_ch / pf_frames / 1000));
        pf_sh = pf_tk = pf_gl = pf_ch = 0;
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

// Windows of a workspace, back to front. Pinned windows come after all the
// others whatever their z -- above everything -- and by z among themselves.
// Minimised windows are included -- the island still lists them -- so
// drawing callers skip WIN_MIN.
static int stack_above(int a, int b) {
    if (nodes[a].pinned != nodes[b].pinned) return nodes[a].pinned > nodes[b].pinned;
    return nodes[a].z > nodes[b].z;
}

// With `all`, the hidden tabs too: the island lists them, and a saved layout
// keeps them.
static int collect_windows_ex(int ws, int *out, int max, int all) {
    int n = 0;
    for (int i = 0; i < MAX_NODES && n < max; i++)
        if (nodes[i].used && nodes[i].ws == ws && (all || !nodes[i].tab_hidden)) out[n++] = i;
    for (int i = 1; i < n; i++) {            // insertion sort; n is tiny
        int v = out[i], j = i - 1;
        while (j >= 0 && stack_above(out[j], v)) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

static int collect_windows(int ws, int *out, int max) {
    return collect_windows_ex(ws, out, max, 0);
}

static int topmost_window(int ws) {
    int best = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used || nodes[i].ws != ws) continue;
        if (nodes[i].state == WIN_MIN || nodes[i].tab_hidden) continue;
        if (best < 0 || stack_above(i, best)) best = i;
    }
    return best;
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
    nd->x = dx + 80 + offs;
    nd->y = dy + 30 + offs;
    nd->z = ++z_top;

    focused = n;
    layout_window(n);
    pane_init(&panes[pi], icols(nd->w), irows(nd->h));
    layout_window(n);
    wp_dirty = 1;
    str_put(nd->path, path ? path : "", sizeof nd->path);
    str_put(nd->arg, arg ? arg : "", sizeof nd->arg);
    if (path) {
        panes[pi].app_pane = 1;
        spawn_prog_in(pi, path, arg);
        busy_start(pi);
        recent_note(path, arg);
    }
    else      { panes[pi].app_pane = 0; spawn_shell_in(pi); }
    if (!quiet_open) anim_appear(AN_OPEN, n);
    return panes[pi].owner_pid;
}

static void new_window(void) { new_window_run(0, 0); }

// The "+" on a frame: a new terminal, as a tab of it.
static void new_tab_in(int n) {
    quiet_open = 1;
    int pid = new_window_run(0, 0);
    quiet_open = 0;
    if (pid < 0 || focused < 0 || focused == n) return;
    tab_join(focused, n);
    relayout();
    refresh_leaves();
}

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
static void close_window(int n) {
    if (n < 0 || !nodes[n].used) return;
    int pi = nodes[n].pane_idx;
    int owner = panes[pi].owner_pid;

    if (owner > 0) {
        int fg = process_foreground(owner);
        if (fg > 0 && fg != owner) {
            process_kill(fg);            // a program inside the terminal
            return;
        }
    }

    // The window really is going -- the last one too: an empty desktop is
    // allowed. Kill what it was running, or the process
    // survives with nowhere left to draw: an orphan that keeps being
    // scheduled and that only the task manager can even see. A tab among
    // others goes without the animation: its frame stays.
    if (!nodes[n].tab_hidden && tab_count(n) == 1) anim_vanish(AN_CLOSE, n);
    if (owner > 0) process_kill(owner);
    panes[pi].alive = 0;
    if (panes[pi].gfx) { kfree(panes[pi].gfx); panes[pi].gfx = 0; }
    panes[pi].gfx_on = 0;
    panes[pi].gfx_w = panes[pi].gfx_h = 0;
    panes[pi].gfx_pid = 0;
    int was_focused = focused == n;
    free_node(n);                        // a tab beside it comes forward
    if (was_focused || (focused >= 0 && !nodes[focused].used)) {
        // The tab that took its place, or else the window behind.
        focused = topmost_window(cur_ws);
    }
    relayout();
}

static void close_focused(void) { close_window(focused); }

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
    anim_vanish(AN_MIN, focused);
    nodes[focused].state = WIN_MIN;
    focused = topmost_window(cur_ws);
    relayout();
}

static void restore_window(int n) {
    if (n < 0 || !nodes[n].used) return;
    if (nodes[n].tab_hidden) tab_show(n);   // its tab comes forward
    int was_min = nodes[n].state == WIN_MIN;
    if (was_min) nodes[n].state = WIN_NORMAL;
    raise_window(n);
    focused = n;
    relayout();
    if (was_min) anim_appear(AN_RESTORE, n);
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
#define HIT_PINBTN 6
#define HIT_TABX   7       // the close mark on one of the window's tabs
#define HIT_TAB    8       // a tab itself: pick it, or take hold of it
#define HIT_PLUS   9       // the "+" after the tabs
#define HIT_L      0x10
#define HIT_R      0x20
#define HIT_T      0x40
#define HIT_B      0x80

// The button a hit is, as WB_* (the tab's close mark is WB_TAB), or -1.
static int hit_button(int hit) {
    switch (hit) {
    case HIT_PINBTN: return WB_PIN;
    case HIT_MINBTN: return WB_MIN;
    case HIT_MAXBTN: return WB_MAX;
    case HIT_CLOSE:  return WB_CLOSE;
    case HIT_TABX:   return WB_TAB;
    case HIT_PLUS:   return WB_PLUS;
    default:         return -1;
    }
}

static int hit_test(int n, int mx, int my) {
    const struct wm_node *nd = &nodes[n];
    int x0 = (int)nd->x, y0 = (int)nd->y;
    int x1 = x0 + (int)nd->w, y1 = y0 + (int)nd->h;
    if (mx < x0 || my < y0 || mx >= x1 || my >= y1) return HIT_NONE;

    // The buttons first: they sit close to the top edge, where a resize
    // handle would otherwise claim them.
    static const int hits[WB_N] = { HIT_PINBTN, HIT_MINBTN, HIT_MAXBTN, HIT_CLOSE };
    for (int b = 0; b < WB_N; b++) {
        int bx, by, bw, bh;
        cap_rect(nd, b, &bx, &by, &bw, &bh);
        if (mx >= bx - 2 && mx < bx + bw + 2 && my >= by - 2 && my < by + bh + 2) return hits[b];
    }
    {
        int ti, t = tab_hit(n, mx, my, &ti);
        if (t == TABH_CLOSE) return HIT_TABX;
        if (t == TABH_PLUS) return HIT_PLUS;
        if (t == TABH_TAB) return HIT_TAB;
    }

    if (nd->state != WIN_MAX) {              // resize handles: the glass edges
        int edge = 0;
        if (mx < x0 + FRAME) edge |= HIT_L;
        if (mx >= x1 - FRAME) edge |= HIT_R;
        if (my < y0 + GRAB) edge |= HIT_T;
        if (my >= y1 - FRAME) edge |= HIT_B;
        // A corner is a corner for a little way along both edges.
        if (edge == HIT_L || edge == HIT_R) {
            if (my < y0 + 2 * FRAME) edge |= HIT_T;
            if (my >= y1 - 2 * FRAME) edge |= HIT_B;
        }
        if (edge) return edge;
    }

    if (my < y0 + TITLE_H) return HIT_TITLE;
    return HIT_CLIENT;
}

// What on a window's top the pointer is over, as the key it lights up by:
// a button, the "+", a tab behind, a tab's close mark. -1 for none.
static int chrome_key(int n, int mx, int my) {
    int ti, t = tab_hit(n, mx, my, &ti);
    if (t == TABH_CLOSE) return KEY_TAB(n, ti, 1);
    if (t == TABH_PLUS) return KEY_CAP(n, WB_PLUS);
    if (t == TABH_TAB) return ti == tab_index(n) ? -1 : KEY_TAB(n, ti, 0);
    int b = hit_button(hit_test(n, mx, my));
    return b >= 0 && b != WB_TAB ? KEY_CAP(n, b) : -1;
}

// The window whose row of tabs is under the pointer, for a tab let go there:
// not one of the group being dragged, and not one with no room for more.
static int strip_target(int mx, int my, int grp) {
    int win[MAX_NODES];
    int c = collect_windows(cur_ws, win, MAX_NODES);
    for (int i = c - 1; i >= 0; i--) {
        const struct wm_node *nd = &nodes[win[i]];
        if (nd->state == WIN_MIN) continue;
        if (mx < (int)nd->x || my < (int)nd->y || mx >= (int)(nd->x + nd->w) ||
            my >= (int)(nd->y + nd->h)) continue;
        if (nd->grp == grp) {                  // the dragged frame itself:
            if (my < (int)nd->y + TITLE_H) return -1;   // its own tabs
            continue;
        }
        if (my >= (int)nd->y + TITLE_H) return -1;   // its body, not its tabs
        return tab_count(win[i]) < TAB_MAX ? win[i] : -1;
    }
    return -1;
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
static int press_tab = -1;                   // the tab whose close mark was pressed
static int drag_tab = 0;                     // the drag is a tab out of a frame of several

// The pointer for a resize handle: which way the edge or corner moves.
static int edge_cursor(int edge) {
    int l = edge & HIT_L, r = edge & HIT_R, t = edge & HIT_T, b = edge & HIT_B;
    if ((l && t) || (r && b)) return CUR_NWSE;
    if ((r && t) || (l && b)) return CUR_NESW;
    if (l || r) return CUR_EW;
    return CUR_NS;
}

static int cursor_pick(int mx, int my) {
    if (busy_active()) return CUR_APPSTART;
    if (drag_mode == DRAG_RESIZE) return edge_cursor(drag_edge);
    if (drag_mode != DRAG_NONE || popup_open() || wm_message_pending() || udrag_active)
        return CUR_ARROW;
    if (my >= (int)fb_get_height() - TASKBAR_H) return CUR_ARROW;
    int n = window_at(mx, my);
    if (n >= 0) {
        int h = hit_test(n, mx, my);
        if (h & 0xF0) return edge_cursor(h);
    }
    return CUR_ARROW;
}

// Switch to workspace `n`. An empty one stays empty, like any desktop.
static void switch_ws(int n) {
    if (n < 0 || n >= MAX_WS || n == cur_ws) return;
    ws_focused[cur_ws] = focused;
    cur_ws = n;

    int top = topmost_window(cur_ws);
    if (top < 0) {
        focused = -1;
        relayout();
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

// Selecting text in a terminal, with the left button: where the press was,
// and the cell under the pointer now. Let go, it is on the clipboard.
static int tsel_node = -1;
static uint16_t tsel_ar, tsel_ac;

static void term_copy(struct pane *p) {
    static char buf[PANE_MAX_ROWS * (PANE_MAX_COLS * 3 + 1)];
    uint32_t n = 0;
    for (uint32_t r = p->sel_r0; r <= p->sel_r1 && r < p->rows; r++) {
        uint32_t c0 = r == p->sel_r0 ? p->sel_c0 : 0;
        uint32_t c1 = r == p->sel_r1 ? p->sel_c1 : p->cols;
        uint32_t line0 = n, last = n;
        for (uint32_t c = c0; c < c1 && c < p->cols; c++) {
            int cp; uint8_t a;
            pane_cell(p, r, c, &cp, &a);
            if (cp == 0) cp = ' ';
            if (cp < 0x80) buf[n++] = (char)cp;
            else if (cp < 0x800) { buf[n++] = (char)(0xC0 | (cp >> 6)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
            else {
                buf[n++] = (char)(0xE0 | (cp >> 12));
                buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            }
            if (cp != ' ') last = n;
        }
        (void)line0;
        n = last;                                    // a line's trailing blanks are not text
        if (r != p->sel_r1) buf[n++] = '\n';
    }
    if (n) clip_set(CLIP_TEXT, buf, n);
}

static void term_select(int n, const struct mouse_event *me) {
    struct pane *p = &panes[nodes[n].pane_idx];
    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    int col = (me->x - (int)cx) / GW, row = (me->y - (int)cy) / GH;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col > (int)p->cols) col = (int)p->cols;
    if (row >= (int)p->rows) row = (int)p->rows - 1;
    if (me->pressed & MOUSE_LEFT) {
        tsel_node = n;
        tsel_ar = (uint16_t)row; tsel_ac = (uint16_t)col;
        p->sel_on = 0;
        dirty = 1;
        return;
    }
    if (tsel_node != n) return;
    if (me->buttons & MOUSE_LEFT) {
        // from the press to here, in reading order
        int r0 = tsel_ar, c0 = tsel_ac, r1 = row, c1 = col;
        if (r1 < r0 || (r1 == r0 && c1 < c0)) { int t = r0; r0 = r1; r1 = t; t = c0; c0 = c1; c1 = t; }
        p->sel_r0 = (uint16_t)r0; p->sel_c0 = (uint16_t)c0;
        p->sel_r1 = (uint16_t)r1; p->sel_c1 = (uint16_t)c1;
        p->sel_on = r1 > r0 || c1 > c0;
        dirty = 1;
        return;
    }
    if (me->released & MOUSE_LEFT) {
        tsel_node = -1;
        if (p->sel_on) term_copy(p);
    }
}

// The clipboard, typed into a terminal: text as it is, files as their paths.
static void term_paste(struct pane *p) {
    static char buf[16384];
    int type;
    uint32_t n = clip_get(buf, sizeof buf - 1, &type);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    buf[n] = 0;
    const char *s = buf;
    if (type == CLIP_FILES) { const char *nl = buf; while (*nl && *nl != '\n') nl++; s = *nl ? nl + 1 : nl; }
    struct kbd_event ev;
    ev.mods = 0;
    ev.pressed = 1;
    for (; *s; s++) {
        if (*s == '\r') continue;
        if (*s == '\n') {
            // a list of files goes on one line; text keeps its lines
            if (type == CLIP_FILES) { if (s[1]) { ev.code = KEY_CHAR; ev.ascii = ' '; pane_push_event(p, &ev); } continue; }
            ev.code = KEY_ENTER; ev.ascii = '\n';
        } else { ev.code = KEY_CHAR; ev.ascii = *s; }
        pane_push_event(p, &ev);
    }
    if (p->owner_pid > 0) process_wake_key();
}

static void pane_push_mouse(int n, const struct mouse_event *me) {
    struct pane *p = &panes[nodes[n].pane_idx];
    if (!p->gfx_on) {
        term_select(n, me);
        if (me->pressed & MOUSE_MIDDLE) term_paste(p);
    }
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
    if (nodes[n].ws != cur_ws || nodes[n].state == WIN_MIN || nodes[n].tab_hidden) return 0;
    if (win_alpha(n) < 255) return 0;     // it is mixed with what is under it
    if (popup_open() || alttab_active || monitor_on || udrag_active) return 0;
    // Animating, the window is not where it will be.
    if (anim.active) return 0;
    // A program painting its own window would erase the strips without
    // restoring what they saved, and the save would then be garbage.
    if (ol_active) return 0;
    if (snap_hint != SNAP_NONE || wm_message_pending()) return 0;
    if (wp_dirty) return 0;                 // a full repaint is already owed

    uint32_t cx, cy, cw, ch;
    content_rect(n, &cx, &cy, &cw, &ch);
    if (!cw || !ch) return 0;

    // Anything stacked above that reaches the content -- a window, or only
    // its shadow -- would be painted over by the shortcut.
    int cbox[4] = { (int)cx, (int)cy, (int)(cx + cw), (int)(cy + ch) };
    for (int i = 0; i < MAX_NODES; i++) {
        if (i == n || !nodes[i].used || nodes[i].tab_hidden) continue;
        if (nodes[i].ws != cur_ws || nodes[i].state == WIN_MIN) continue;
        if (!stack_above(i, n)) continue;
        int e[4];
        window_extent(i, e);
        if (boxes_meet(e, cbox)) return 0;   // something is stacked over it
    }

    cursor_restore();
    draw_content(n, cx, cy, cw, ch, n == focused);
    repair_body_corners(n);           // the picture squared them off
    cursor_draw();
    fb_present();
    // This frame is on the screen now; the full path need not paint it again.
    struct pane_cache *pc = &pcache[nodes[n].pane_idx];
    if (pc->valid && pc->gfx) pc->gen = gfx_gen[nodes[n].pane_idx];
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
    gfx_gen[p - panes]++;
    busy_end((int)(p - panes));        // it is up: its first picture is here
    {   // Whoever is painting owns the surface for as long as they live.
        process_t *me = process_current();
        p->gfx_pid = me ? me->pid : 0;
    }
    {
        // A tab behind keeps its picture for when it is picked; nothing of
        // it is on the screen to refresh.
        int nn = node_for_pane(p);
        if (nn >= 0 && nodes[nn].tab_hidden) return 0;
        if (!present_pane_only(nn)) wm_refresh();
    }
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

    // A program that ends before drawing anything is not starting any more.
    for (int i = 0; i < MAX_PANES; i++)
        if (panes[i].alive && panes[i].owner_pid == pid) busy_end(i);

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
        // The program the window was for has ended -- a program that
        // exited, a shell somebody typed `exit` in -- so the window goes too.
        // The desktop may be left with no window at all: that is a desktop,
        // not an error, and the menu opens the next thing.
        panes[i].app_pane = 0;
        for (int k = 0; k < MAX_NODES; k++) {
            if (!nodes[k].used || nodes[k].pane_idx != i) continue;
            if (nodes[k].ws == cur_ws && nodes[k].state != WIN_MIN &&
                !nodes[k].tab_hidden && tab_count(k) == 1)
                anim_vanish(AN_CLOSE, k);
            int was_focused = focused == k;
            free_node(k);
            if (was_focused || (focused >= 0 && !nodes[focused].used))
                focused = topmost_window(cur_ws);
            relayout();
            break;
        }
        panes[i].alive = 0;
        wp_dirty = 1;
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
#define WMB_START     0xC0   /* open or close the start menu */

static void start_toggle(void) {
    cc_open = 0;
    am_open = 0;
    cc_vol_drag = 0;
    start_open = !start_open;
    if (start_open) pop_t0 = now_ms();
    start_qlen = 0;
    start_q[0] = 0;
    sm_power_open = 0;
    sm_all = 0;
    pop_glass_fresh = 0;
    if (start_open) {
        start_build();
        sm_build_rows();
        start_sel = sm_first_item();
        start_top = 0;
    }
    wp_dirty = 1;
    dirty = 1;
}

static void start_close(void) {
    if (start_open) start_toggle();
}

// The search changed: a new list, the selection on its first program.
static void start_requery(void) {
    sm_build_rows();
    start_sel = sm_first_item();
    start_top = 0;
    menu_dirty = 1;
    dirty = 1;
}

// Open a program in a new window, from the menu.
static void start_run(const char *path, const char *arg) {
    start_close();
    new_window_run(path, arg);
    refresh_leaves();
    dirty = 1;
}

static void bin_path(char *out, int max, const char *file);

// A click inside the open menu, on whatever sm_key_at() named.
static void start_click(int key) {
    int kind = (key >> 10) & 0x3F, arg = key & 0x3FF;
    if (key < 0) { start_close(); return; }
    char path[48];
    switch (kind) {
    case SMK_ROW:
        if (arg < sm_nrows && sm_rows[arg].item >= 0) {
            const struct start_item *it = &start_items[sm_rows[arg].item];
            if (str_same(it->name_file, "sh")) { start_close(); new_window(); refresh_leaves(); }
            else start_run(it->path, 0);
        }
        break;
    case SMK_PIN:
        if (str_same(ln_pins[arg], "sh")) { start_close(); new_window(); refresh_leaves(); break; }
        bin_path(path, sizeof path, ln_pins[arg]);
        start_run(path, 0);
        break;
    case SMK_REC:
        if (arg < recent_n) {
            char p2[48], a2[96];
            str_put(p2, recents[arg].path, sizeof p2);
            str_put(a2, recents[arg].arg, sizeof a2);
            start_run(p2, a2[0] ? a2 : 0);
        }
        break;
    case SMK_ALL:
        sm_all = !sm_all;
        start_top = 0;
        start_sel = sm_first_item();
        menu_dirty = 1;
        dirty = 1;
        break;
    case SMK_FOOT:
        if (arg == 1) { sm_power_open = !sm_power_open; menu_dirty = 1; dirty = 1; }
        else start_run("/bin/control", 0);
        break;
    case SMK_POWER:
        if (arg == 0) power_reboot();
        else power_off();
        break;
    default:
        // Inside the menu, on nothing: the power choice goes away, the
        // menu stays.
        if (sm_power_open) { sm_power_open = 0; menu_dirty = 1; dirty = 1; }
        break;
    }
}

// A click on the taskbar: whatever tb_layout() put under the pointer.
// The path of a program in /bin.
static void bin_path(char *out, int max, const char *file) {
    str_put(out, "/bin/", max);
    int n = 5, k = 0;
    while (file[k] && n < max - 1) out[n++] = file[k++];
    out[n] = 0;
}

// A click on the island: whatever tb_layout() put under the pointer.
static void taskbar_click(int mx, int my) {
    tb_layout();
    int i = tb_item_at(mx, my);
    if (i < 0) return;
    const struct tb_item *it = &tb_items[i];
    if (it->kind != TB_ORB) start_close();   // anything else on the island closes it
    if (it->kind != TB_STATUS && cc_open) { cc_open = 0; wp_dirty = 1; }
    if (am_open) { am_open = 0; wp_dirty = 1; }
    switch (it->kind) {
    case TB_ORB:
        start_toggle();
        break;
    case TB_STATUS: {
        // The layout's label switches the layout; the rest of the pill -- the
        // clock -- opens the control panel, and closes it again.
        int lx, ly;
        lang_chip_at(it, &lx, &ly);
        if (mx >= lx - 3 && mx < lx + LANG_CHIP_W + 3 && my >= ly - 3 && my < ly + LANG_CHIP_H + 3) {
            kbd_toggle();
            break;
        }
        cc_open = !cc_open;
        if (cc_open) pop_t0 = now_ms();
        cc_vol_drag = 0;
        pop_glass_fresh = 0;
        wp_dirty = 1;
        dirty = 1;
        break;
    }
    case TB_APP: {
        int n = it->win;
        if (n >= 0 && nodes[n].used) {
            // Its window in front: tuck it away. Otherwise bring it forward.
            if (n == focused && nodes[n].state != WIN_MIN) minimize_focused();
            else restore_window(n);
        } else if (str_same(it->name, "sh")) {
            new_window();
        } else {
            char path[48];
            bin_path(path, sizeof path, it->name);
            new_window_run(path, 0);
        }
        refresh_leaves();
        dirty = 1;
        break;
    }
    default:
        break;
    }
}

// A click in a program's list of windows.
static void am_click(int key) {
    int kind = (key >> 8) & 0xFF, arg = key & 0xFF;
    am_collect();
    switch (kind) {
    case AMK_CARD:
        if (arg < am_n) {
            am_open = 0;
            restore_window(am_win[arg]);
            refresh_leaves();
        }
        break;
    case AMK_CLOSE:
        // The list stays up, one window shorter.
        if (arg < am_n) close_window(am_win[arg]);
        break;
    case AMK_ALL: {
        int w[AM_MAXW], k = am_n;
        for (int i = 0; i < k; i++) w[i] = am_win[i];
        for (int i = 0; i < k; i++) if (nodes[w[i]].used) close_window(w[i]);
        am_open = 0;
        break;
    }
    case AMK_NEW:
        am_open = 0;
        if (str_same(am_name, "sh")) {
            new_window();
        } else {
            char path[48];
            bin_path(path, sizeof path, am_name);
            new_window_run(path, 0);
        }
        refresh_leaves();
        break;
    default:
        break;
    }
    // The list may have moved under the pointer: light what is there now.
    {
        int32_t px, py;
        mouse_position(&px, &py);
        int k = am_open ? am_key_at(px, py) : -1;
        hover_to(k == KEY_AM(AMK_INSIDE, 0) ? -1 : k);
    }
    wp_dirty = 1;
    dirty = 1;
    tb_valid = 0;
}

// Dragging the island by its glass: let go near an edge and it moves there.
static int isl_drag = 0, isl_drag_x, isl_drag_y;

static void island_set_edge(int e) {
    if (e == island_edge) return;
    island_edge = e;
    isl_keep.have = 0;
    relayout();
    refresh_leaves();
    wp_dirty = 1;
    dirty = 1;
    desk_save();
}

static void island_drop(int mx, int my) {
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    int edge = island_edge;
    // Whichever edge the pointer is nearest, by the proportion of the
    // screen, so a short tug does not move it.
    int dl = mx, dr = W - mx, dt = my, db = H - my;
    int best = db, e = ISL_BOTTOM;
    if (dl * H < best * W) { best = dl * H / W; e = ISL_LEFT; }
    if (dr * H < best * W) { best = dr * H / W; e = ISL_RIGHT; }
    if (dt < best) { e = ISL_TOP; }
    if (e != edge) island_set_edge(e);
}

// Act on one pointer event: press starts a drag or arms a button, motion
// carries a drag, release completes whatever the press began.
static void handle_mouse(const struct mouse_event *me) {
    int mx = me->x, my = me->y;
    uint32_t H = fb_get_height();
    (void)H;
    tb_layout();
    int on_taskbar = on_island(mx, my);

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

    // What the pointer is over, so it can light up: a caption button or
    // anything on the taskbar.
    if (drag_mode == DRAG_NONE && !wm_message_pending()) {
        int key = -1;
        if (on_taskbar) {
            int i = tb_item_at(mx, my);
            if (i >= 0) key = KEY_TB(i);
        } else if (start_open) {
            key = sm_key_at(mx, my);
            if (key == KEY_SM(SMK_INSIDE, 0)) key = -1;
        } else if (cc_open) {
            key = cc_key_at(mx, my);
            if (key == KEY_CC(CCK_INSIDE, 0)) key = -1;
        } else if (am_open) {
            key = am_key_at(mx, my);
            if (key == KEY_AM(AMK_INSIDE, 0)) key = -1;
        } else {
            int n = window_at(mx, my);
            if (n >= 0) key = chrome_key(n, mx, my);
        }
        if (hover_to(key)) { dirty = 1; menu_dirty = 1; }
    }

    // The volume knob follows the pointer while it is held.
    if (cc_open && cc_vol_drag) {
        if (me->buttons & MOUSE_LEFT) cc_set_volume_at(mx);
        if (me->released & MOUSE_LEFT) { cc_vol_drag = 0; menu_dirty = 1; dirty = 1; }
        return;
    }
    // The wheel over the panel turns the volume up and down.
    if (cc_open && me->wheel && !on_taskbar) {
        if (cc_key_at(mx, my) >= 0) {
            int v = audio_volume() + (me->wheel > 0 ? 5 : -5);
            audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
            menu_dirty = 1;
            dirty = 1;
            tb_valid = 0;
        }
        return;
    }

    // The wheel scrolls the menu's list, three rows a notch.
    if (start_open && me->wheel && !on_taskbar) {
        int lx, ly, lw, lh;
        sm_list_box(&lx, &ly, &lw, &lh);
        if (mx >= lx && mx < lx + lw) {
            start_top += me->wheel > 0 ? -3 : 3;
            if (start_top > sm_nrows - 4) start_top = sm_nrows - 4;
            if (start_top < 0) start_top = 0;
            menu_dirty = 1;
            dirty = 1;
        }
        return;
    }

    // The wheel on the pin of a pinned window: how much of it shows.
    if (me->wheel && drag_mode == DRAG_NONE && !on_taskbar && !popup_open()) {
        int n = window_at(mx, my);
        if (n >= 0 && nodes[n].pinned && hit_test(n, mx, my) == HIT_PINBTN) {
            int a = nodes[n].alpha + (me->wheel > 0 ? 26 : -26);
            nodes[n].alpha = a < 77 ? 77 : a > 255 ? 255 : a;
            dirty = 1;
            return;
        }
    }

    // Pointer motion and wheel over the focused window's interior belong to
    // the program, not to the WM. Button transitions fall through below --
    // those may start a drag or hit window chrome first.
    if (drag_mode == DRAG_NONE && !on_taskbar && !popup_open() && !me->pressed && !me->released) {
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
            drag_tab = 0;
            join_hint = -1;
            return;
        }
        int dx = mx - drag_gx, dy = my - drag_gy;
        int nx, ny, nw, nh;
        if (drag_mode == DRAG_MOVE) {
            nx = (int)drag_ox + dx; ny = (int)drag_oy + dy;
            nw = (int)drag_ow;      nh = (int)drag_oh;
            if (!drag_tab) shake_track(mx, drag_win);
            // Over another frame's tabs: it would join them -- show where.
            int jt = (dx || dy) ? strip_target(mx, my, nodes[drag_win].grp) : -1;
            if (jt != join_hint) { join_hint = jt; dirty = 1; }
            int hint = jt >= 0 ? SNAP_NONE : snap_from_pointer(mx, my);
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
        if (on_taskbar) {
            // On a program: its windows, to pick from, close, or add to.
            if (me->pressed & MOUSE_RIGHT) {
                int i = tb_item_at(mx, my);
                if (i >= 0 && tb_items[i].kind == TB_APP) am_show(tb_items[i].name);
                else if (am_open) { am_open = 0; wp_dirty = 1; dirty = 1; }
            }
            return;
        }
        if (am_open) {
            if (me->pressed & MOUSE_RIGHT) {
                int in = am_key_at(mx, my) >= 0;
                am_open = 0;
                wp_dirty = 1;
                dirty = 1;
                if (in) return;
            } else if (am_key_at(mx, my) >= 0) {
                return;
            }
        }
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

        // The start menu is modal for the pointer: a press inside arms what
        // it is on (it fires on release, over the same thing), a press
        // anywhere else dismisses it.
        // A notice: a click puts it away.
        if (note_hit(mx, my)) {
            // A notice about a drive opens it.
            if (note_n && notes[0].open[0]) new_window_run("/bin/files", notes[0].open);
            note_t0 = now_ms() - NOTE_SHOW;
            dirty = 1;
            return;
        }

        if (start_open && !on_taskbar) {
            int key = sm_key_at(mx, my);
            if (key < 0) { start_close(); return; }
            press_key = key;
            press_win = -3;
            menu_dirty = 1;
            return;
        }
        // The control panel: the volume is taken hold of at once; anything
        // else fires on release, over the same thing. Outside, it closes.
        if (cc_open && !on_taskbar) {
            int key = cc_key_at(mx, my);
            if (key < 0) { cc_open = 0; wp_dirty = 1; dirty = 1; return; }
            if (key == KEY_CC(CCK_VOL, 0)) { cc_vol_drag = 1; cc_set_volume_at(mx); return; }
            press_key = key;
            press_win = -4;
            menu_dirty = 1;
            return;
        }
        // A program's windows: anything in it fires on release; outside, it goes.
        if (am_open && !on_taskbar) {
            int key = am_key_at(mx, my);
            if (key < 0) { am_open = 0; wp_dirty = 1; dirty = 1; return; }
            press_key = key;
            press_win = -5;
            menu_dirty = 1;
            return;
        }

        if (on_taskbar) {
            press_hit = HIT_CLIENT; press_win = -2;
            int i = tb_item_at(mx, my);
            press_key = i >= 0 ? KEY_TB(i) : -1;
            // A press on the glass itself, between things: the island is
            // being picked up, to be put down at another edge.
            if (i < 0) { isl_drag = 1; isl_drag_x = mx; isl_drag_y = my; }
            return;
        }

        int n = window_at(mx, my);
        if (n < 0) return;                       // press on the bare desktop
        restore_window(n);                       // focus + raise, like Windows
        int hit = hit_test(n, mx, my);
        press_hit = hit; press_win = n;
        press_tab = -1;
        if (hit == HIT_TABX) {
            // Which tab's close mark: it fires on release over the same one.
            int ti;
            tab_hit(n, mx, my, &ti);
            int m[TAB_MAX];
            tab_members(n, m);
            press_tab = m[ti];
            press_key = KEY_TAB(n, ti, 1);
            return;
        }
        if (hit_button(hit) >= 0) {
            press_key = KEY_CAP(n, hit_button(hit));
            return;
        }
        drag_tab = 0;
        if (hit == HIT_TAB) {
            // A tab: picked at once, and a handle too -- to drag the window
            // by, or, among others, to pull it out on its own.
            int ti;
            tab_hit(n, mx, my, &ti);
            int m[TAB_MAX], k = tab_members(n, m);
            if (m[ti] != n) {
                tab_show(m[ti]);
                n = m[ti];
                focused = n;
                relayout();
                refresh_leaves();
                press_win = n;
            }
            drag_tab = k > 1;
        }

        if (hit == HIT_CLIENT) { pane_push_mouse(n, me); return; }

        if (hit == HIT_TITLE || hit == HIT_TAB || (hit & 0xF0)) {
            struct wm_node *nd = &nodes[n];
            drag_win = n;
            drag_gx = mx; drag_gy = my;
            drag_ox = nd->x; drag_oy = nd->y; drag_ow = nd->w; drag_oh = nd->h;
            if (drag_tab && nd->state == WIN_MAX) {
                // Pulled out of a full-screen frame, a tab comes out at the
                // size the frame had before it filled the screen.
                drag_ow = nd->sw; drag_oh = nd->sh;
                drag_ox = (uint32_t)(mx > 120 ? mx - 120 : 0);
                drag_oy = (uint32_t)(my > 20 ? my - 20 : 0);
            }
            if (hit == HIT_TITLE || hit == HIT_TAB) {
                if (nd->state == WIN_MAX && !drag_tab) return;   // a maximised window stays put
                drag_mode = DRAG_MOVE;
                shake_reset(mx);
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
        int was_key = press_key;
        if (press_key >= 0) { press_key = -1; dirty = 1; tb_valid = 0; }
        if (drag_mode != DRAG_NONE) {
            int moved = ol_active;
            outline_off();          // clean desktop before the window moves
            struct wm_node *nd = (drag_win >= 0 && nodes[drag_win].used)
                               ? &nodes[drag_win] : 0;
            if (moved && nd && drag_mode == DRAG_MOVE) {
                int tgt = strip_target(mx, my, nd->grp);
                int own = my >= (int)nd->y && my < (int)nd->y + TITLE_H &&
                          mx >= (int)nd->x && mx < (int)(nd->x + nd->w);
                if (tgt >= 0) {
                    // Let go on another frame's tabs: it joins them.
                    if (drag_tab) tab_join(drag_win, tgt);
                    else group_join(drag_win, tgt);
                    relayout();
                    refresh_leaves();
                    nd = 0;
                } else if (drag_tab && own) {
                    // Along its own row: the tab moves to where it was let go.
                    int ti, t = tab_hit(drag_win, mx, my, &ti);
                    if (t == TABH_TAB || t == TABH_CLOSE) tab_move_to(drag_win, ti);
                    nd = 0;
                } else if (drag_tab) {
                    // Anywhere else: out of its frame, a window of its own,
                    // which the outline then places like any other.
                    tab_leave(drag_win);
                    nd->state = WIN_NORMAL;
                    raise_window(drag_win);
                    focused = drag_win;
                }
            }
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
            drag_tab = 0;
            join_hint = -1;
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
        if (press_win == -3) {
            if (start_open && sm_key_at(mx, my) == was_key) start_click(was_key);
            press_win = -1;
            menu_dirty = 1;
            return;
        }
        if (press_win == -4) {
            if (cc_open && cc_key_at(mx, my) == was_key) cc_click(was_key);
            press_win = -1;
            menu_dirty = 1;
            return;
        }
        if (press_win == -5) {
            if (am_open && am_key_at(mx, my) == was_key) am_click(was_key);
            press_win = -1;
            menu_dirty = 1;
            return;
        }
        if (press_win == -2) {
            if (isl_drag) {
                // Put down far enough from where it was picked up: it moves.
                int dx = mx - isl_drag_x, dy = my - isl_drag_y;
                isl_drag = 0;
                if (dx * dx + dy * dy > 80 * 80) island_drop(mx, my);
                press_win = -1;
                return;
            }
            // Only if it comes up over what it went down on.
            tb_layout();
            int i = on_taskbar ? tb_item_at(mx, my) : -1;
            if (i >= 0 && KEY_TB(i) == was_key) taskbar_click(mx, my);
            press_win = -1;
            return;
        }
        if (press_win >= 0 && nodes[press_win].used &&
            hit_test(press_win, mx, my) == press_hit) {
            focused = press_win;
            if (press_hit == HIT_TITLE || press_hit == HIT_TAB) {
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
                case HIT_TABX:
                    // Only if it comes up over the same tab's mark.
                    if (chrome_key(press_win, mx, my) == was_key) close_window(press_tab);
                    break;
                case HIT_PLUS:   new_tab_in(press_win); break;
                case HIT_MAXBTN: toggle_maximize(); break;
                case HIT_MINBTN: minimize_focused(); break;
                case HIT_PINBTN:
                    // Pinned: above everything else, until unpinned.
                    nodes[press_win].pinned = !nodes[press_win].pinned;
                    wp_dirty = 1;
                    break;
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
        case 5:                                  // the pointer on and off a button
            if (f >= 0 && nodes[f].used) {
                hover_to((frames & 1) ? KEY_CAP(f, WB_CLOSE) : -1);
                hov_ms = now_ms() - FADE_MS;      // lit at once: every frame differs
                hov_old_ms = hov_ms;
            }
            render_all();
            break;
        case 6: {                                // typing into a terminal
            int tn = -1;
            for (int i = 0; i < MAX_NODES; i++)
                if (nodes[i].used && !nodes[i].tab_hidden && !panes[nodes[i].pane_idx].gfx_on) tn = i;
            if (tn >= 0) {
                struct pane *tp = &panes[nodes[tn].pane_idx];
                pane_putc(tp, (char)('a' + (frames % 26)));
                if ((frames % 60) == 59) pane_putc(tp, '\n');
            }
            render_all();
            break;
        }
        case 7:                                  // the control panel opened and closed
            cc_open = !cc_open;
            pop_t0 = now_ms() - SPRING_MS - 50;  // no spring: the open and close themselves
            pop_glass_fresh = 0;
            wp_dirty_on_popup();
            render_all();
            break;
        case 8: {                                // focus to and fro between two windows
            int win[MAX_NODES], n = collect_windows(cur_ws, win, MAX_NODES);
            if (n >= 2) restore_window(win[0]);
            render_all();
            break;
        }
        case 12:                                 // the launcher springing up, mid-way
            if (!start_open) { start_toggle(); render_all(); }
            pop_t0 = now_ms() - 120;                // in the middle of its spring
            menu_dirty = 1;
            pop_glass_fresh = 0;
            render_all();
            break;
        case 13:                                 // a window opening: one frame of it
            if (f >= 0 && nodes[f].used) {
                if (!anim.active) anim_appear(AN_OPEN, f);
                anim.t0 = now_ms() - 100;            // mid-way, every frame
                anim.frames = 1;
            }
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
        case 9:                                  // every pixel changed, out to the screen
            fb_fill_rect(0, 0, fb_get_width(), fb_get_height(),
                         (frames & 1) ? 0x00203040 : 0x00304050);
            fb_present();
            break;
        case 10:                                 // a 1-pixel column, top to bottom
            fb_fill_rect(700, 0, 1, fb_get_height(), (frames & 1) ? 0x00203040 : 0x00304050);
            fb_present();
            break;
        case 11:                                 // a 600-pixel row band, 40 rows
            fb_fill_rect(300, 500, 600, 40, (frames & 1) ? 0x00203040 : 0x00304050);
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
    // The timer has to keep ticking for any of this to be measured, and the
    // scheduler loop that runs the desktop does so with interrupts off.
    __asm__ volatile ("sti");
    kprintf("fps: measuring frames DELIVERED, one second per phase (%d cores)\n", smp_cpu_count());
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
    bench_phase("hover", 5, 100);
    bench_phase("typing", 6, 100);
    bench_phase("panel open/close", 7, 100);
    cc_open = 0; wp_dirty = 1; render_all();
    bench_phase("focus switch", 8, 100);
    bench_phase("launcher spring", 12, 100);
    if (start_open) start_toggle();
    bench_phase("window opening", 13, 100);
    anim_stop();
    bench_phase("screen push", 3, 100);
    bench_phase("screen write", 9, 100);
    bench_phase("column 1x1080", 10, 100);
    bench_phase("band 600x40", 11, 100);
    bench_phase("ram fill", 4, 100);
    wp_dirty = 1;
    dirty = 1;
    kprintf("fps: done\n");
    __asm__ volatile ("cli");
}

void wm_route_input(void) {
    // Before the desktop is up the keys stay where they are: the boot screen
    // looks at them (a key shows the log), and wm_start throws the rest away.
    if (!started) return;
    struct kbd_event ev;
    while (keyboard_poll_event(&ev)) {
        // Key-up, and the modifier keys as keys, exist for programs that track
        // what is HELD. They never mean a shortcut -- releasing Alt+F4 must not
        // close a second window -- and they only reach a program that asked for
        // them. Everything below this point may assume a key going down.
        // Alt+Shift switches the layout when the second of the two is let go
        // with nothing pressed in between (Alt+Shift+Tab is not a switch).
        if (ev.pressed && ((ev.code == KEY_SHIFT && (ev.mods & KBD_MOD_ALT)) ||
                           (ev.code == KEY_ALT && (ev.mods & KBD_MOD_SHIFT))))
            kbd_chord = 1;
        else if (ev.pressed && ev.code != KEY_SHIFT && ev.code != KEY_ALT)
            kbd_chord = 0;
        else if (!ev.pressed && kbd_chord && (ev.code == KEY_SHIFT || ev.code == KEY_ALT)) {
            kbd_chord = 0;
            kbd_toggle();
        }
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
        // Opening reads /bin from the disk, so it waits for wm_poll.
        if (ev.code == KEY_ESC && ctrl) { push_binding(WMB_START); continue; }

        // The control panel takes Escape to close and nothing else.
        if (cc_open && ev.code == KEY_ESC) { cc_open = 0; wp_dirty = 1; dirty = 1; continue; }
        if (am_open && ev.code == KEY_ESC) { am_open = 0; wp_dirty = 1; dirty = 1; continue; }

        // While the menu is up it owns the keyboard: no program should
        // receive the letters someone is typing into a search box. Each key
        // changes only the menu, which redraws over the desktop it saved.
        if (start_open) {
            if (ev.code == KEY_ESC) {
                if (sm_power_open) { sm_power_open = 0; menu_dirty = 1; dirty = 1; }
                else push_binding(WMB_START);
            } else if (ev.code == KEY_ENTER) {
                push_binding(WMB_LAUNCH);
            } else if (ev.code == KEY_UP) {
                sm_step(-1); sm_scroll_to_sel(); menu_dirty = 1; dirty = 1;
            } else if (ev.code == KEY_DOWN) {
                sm_step(1); sm_scroll_to_sel(); menu_dirty = 1; dirty = 1;
            } else if (ev.code == KEY_BKSP) {
                if (start_qlen) start_q[--start_qlen] = 0;
                start_requery();
            } else if (ev.code == KEY_CHAR && (unsigned char)ev.ascii >= 32 &&
                       (unsigned char)ev.ascii < 127) {
                if (start_qlen < (int)sizeof start_q - 1) {
                    start_q[start_qlen++] = ev.ascii;
                    start_q[start_qlen] = 0;
                }
                start_requery();
            }
            continue;
        }

        // --- Windows-style window management ---
        if (ev.code == KEY_CHAR && ev.ascii == ' ' && super) { kbd_toggle(); continue; }
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

        // Ctrl+Shift+V in a terminal: the clipboard, typed in.
        if (!p->gfx_on && ev.code == KEY_CHAR && (ev.mods & KBD_MOD_CTRL) && (ev.mods & KBD_MOD_SHIFT) &&
            (ev.ascii == 'v' || ev.ascii == 'V')) {
            term_paste(p);
            continue;
        }
        // Ctrl+Shift+C: the selection, onto it (it is there already after a
        // drag; this is for the hand that expects it).
        if (!p->gfx_on && ev.code == KEY_CHAR && (ev.mods & KBD_MOD_CTRL) && (ev.mods & KBD_MOD_SHIFT) &&
            (ev.ascii == 'c' || ev.ascii == 'C')) {
            if (p->sel_on) term_copy(p);
            continue;
        }
        if (p->sel_on) { p->sel_on = 0; dirty = 1; }

        // Any real input jumps back to the live bottom -- you type, you see the
        // prompt. (Modifier-only auto-repeats carry no code, but those never
        // reach here.)
        if (p->scroll != 0) { pane_scroll_reset(p); dirty = 1; }

        // Ordinary input belongs to whoever owns the focused pane -- in the
        // layout that is on.
        uint32_t cp = 0;
        if (kbd_ru && ev.code == KEY_CHAR && !(ev.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_SUPER)))
            cp = ru_of(ev.ascii);
        if (cp >= 0x80) {
            struct kbd_event u = ev;
            uint8_t b[3];
            int nb = cp < 0x800 ? 2 : 3;
            if (nb == 2) { b[0] = (uint8_t)(0xC0 | (cp >> 6)); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); }
            else {
                b[0] = (uint8_t)(0xE0 | (cp >> 12));
                b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                b[2] = (uint8_t)(0x80 | (cp & 0x3F));
            }
            for (int k = 0; k < nb; k++) { u.ascii = (char)b[k]; pane_push_event(p, &u); }
        } else {
            if (cp) ev.ascii = (char)cp;
            pane_push_event(p, &ev);
        }
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
            if (!start_open) break;
            int it = (start_sel >= 0 && start_sel < sm_nrows) ? sm_rows[start_sel].item : -1;
            if (it >= 0) start_run(start_items[it].path, 0);
            break;
        }
        case WMB_START:
            start_toggle();
            break;
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

int wm_running(void) { return started; }
int wm_lang(void) { return lang; }

// Something is moving -- a window opening or closing, a popup coming up, a
// notice sliding in -- and its next frame is due. The scheduler asks, and on
// the desktop's core puts a running program down for a moment to draw it:
// otherwise one busy program, alone on that core, starves the motion into a
// slide show.
int wm_wants_frame(void) {
    if (!started || screen_asleep) return 0;
    if (!anim.active && !pop_animating() && !note_animating() && !boot_px) return 0;
    return now_ms() - last_frame_ms >= 8;
}

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

    // The start-up sound, once the desktop is really running: from here the
    // timer ticks, and the sound is looked after as it plays.
    // And after its first frame: drawing that one (the wallpaper is made
    // then) holds the processor longer than the sound ring lasts.
    {
        static int greeted;
        static uint64_t first;
        if (!first && last_frame_ms) first = now_ms();
        if (!greeted && first && now_ms() - first > 150) {
            greeted = 1;
            sound_play(SND_STARTUP);
        }
    }

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

    // A glow still fading in or out wants its next step drawn.
    if (hover_animating()) dirty = 1;
    if (anim.active) dirty = 1;                 // the next step of a window's way
    if (note_animating()) dirty = 1;            // a notice coming or going

    // A USB device plugged in or pulled out: said once, with what it is.
    {
        struct usb_news un;
        while (xhci_news(&un)) usb_note(&un);
    }
    if (pop_animating()) {                      // a popup springing into place
        dirty = 1;
        menu_dirty = 1;
        pop_glass_fresh = 0;                    // it moved: what is under it is new
    }
    // The spinning pointer turns a step every 60 ms, and turns back into the
    // arrow when its time is up.
    {
        static uint64_t cur_anim_ms;
        if (busy_until) {
            uint64_t now = now_ms();
            if (now >= busy_until) { busy_until = 0; busy_pane = -1; pointer_moved = 1; }
            else if (now - cur_anim_ms >= 60) { cur_anim_ms = now; pointer_moved = 1; }
        }
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

// The boot screen, faded out over the desktop's first frames.
static uint64_t boot_t0;
#define BOOT_FADE_MS 450

static void boot_fade(void) {
    if (!boot_px) return;
    uint64_t now = now_ms();
    if (!boot_t0) boot_t0 = now;
    uint64_t el = now - boot_t0;
    if (el >= BOOT_FADE_MS) {
        kfree(boot_px);
        boot_px = 0;
        return;
    }
    int t = (int)(el * 256 / BOOT_FADE_MS);
    int e = 256 - (((256 - t) * (256 - t)) >> 8);       // ease out
    int W = (int)fb_get_width(), H = (int)fb_get_height();
    px_mix(bb_at(0, 0), bb_stride(), boot_px, W, W, H, e);
    fb_mark_rows(0, (uint32_t)H);
    wp_dirty = 1;      // the back buffer is a mixture now: the next frame starts clean
    dirty = 1;
}

void wm_start(void) {
    boot_px = splash_end();   // the boot screen's last picture, to fade from
    keyboard_flush();
    icons_load();             // /icons.bin, if the build produced one
    // The desktop's own face, rasterised while floating point is still ours
    // to use: no program exists yet whose registers it could disturb.
    uifont_init();
    cursor_init();
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

    desk_load();              // language, theme, the island's edge
    started = 1;
    dirty = 1;
    // The windows kept with "keep this layout", or else a first shell.
    if (!layout_restore()) new_window();
}
