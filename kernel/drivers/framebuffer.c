#include "framebuffer.h"
#include "font8x8_basic.h"
#include "../mm/heap.h"
#include <stddef.h>

#define GLYPH_W 8
#define GLYPH_H 8

static volatile uint8_t *fb_mem = NULL;      // the real (front) framebuffer
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint8_t  fb_bpp = 0;

// Double buffering: all drawing goes to `draw_target`. During boot it points
// at the front buffer (so kprintf shows immediately); the WM switches it to an
// off-screen back buffer and blits with fb_present() to avoid flicker.
#define FB_BACK_MAX (1920u * 1080u * 4u)
static uint8_t  fb_back_store[FB_BACK_MAX];
static uint8_t *fb_back = NULL;
static volatile uint8_t *draw_target = NULL;

// A copy of what is currently ON the screen, so a present can skip pushing
// pixels that already look like that.
//
// This exists because of one measurement: writing the whole screen to the
// framebuffer costs 8.3 MB at about 80 MB/s, while writing the same 8.3 MB to
// ordinary RAM runs at 63 GB/s. Video memory is between two and three orders
// of magnitude more expensive per byte than RAM, so spending two RAM READS to
// avoid one framebuffer WRITE is a trade worth taking every time.
//
// An earlier version of this compared whole SCANLINES of the whole screen on
// every frame -- ten megabytes read to avoid ninety kilobytes written -- and
// was rightly deleted. The difference now is that the comparison only ever
// looks inside the rectangles something actually drew into. Damage says WHERE
// to look; the shadow says whether it was worth the bus.
static uint8_t *fb_shadow = NULL;

// What changed since the last present, as a short LIST of rectangles rather
// than one bounding box.
//
// A single box is fine when a frame touches one region. It is terrible when it
// touches two that are far apart: a pointer in one corner and the ticking
// clock in the other force the union of the two, which is most of the screen.
// Dragging a window as an outline is the extreme case -- four thin strips
// erased and four redrawn, whose bounding box is the entire window, i.e.
// exactly the copy the outline exists to avoid.
//
// Sixteen rectangles is plenty; past that they get folded into whichever
// neighbour grows least, which stays correct and only carries some unchanged
// pixels along for the ride.
#define FB_DMG_MAX 16
struct fb_dmg { uint32_t x0, y0, x1, y1; };
static struct fb_dmg dmg[FB_DMG_MAX];
static int dmg_n = 0;

static uint64_t dmg_area(const struct fb_dmg *r) {
    return (uint64_t)(r->x1 - r->x0) * (r->y1 - r->y0);
}

// Merge two rectangles only if the merged one is no bigger than the two of
// them put together -- that is, if the union wastes nothing.
//
// The obvious rule, "merge whatever touches", is wrong here, and wrong in a
// way that quietly undoes the whole exercise. The four strips of a window
// outline touch each other at the corners, so under that rule they collapse
// into the window's bounding box: ten thousand pixels turn back into two
// million and the outline drag is no cheaper than moving the window. Measured,
// that mistake cost a factor of thirty.
//
// Comparing areas gets both cases right. Two glyphs side by side in a line of
// text merge (16x8 is exactly two 8x8s). A horizontal strip and a vertical one
// meeting at a corner do not.
static int dmg_worth_merging(const struct fb_dmg *a, const struct fb_dmg *b) {
    struct fb_dmg u;
    u.x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    u.y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    u.x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    u.y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    return dmg_area(&u) <= dmg_area(a) + dmg_area(b);
}

// Set when kernel logging draws into the WM's back buffer. The compositor uses
// it to repaint the desktop over the text, which is what used to happen for
// free when the wallpaper was redrawn every frame.
static int console_wrote = 0;

void fb_mark_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fb_height || !fb_width) return;
    if (y >= fb_height || x >= fb_width) return;
    uint32_t y2 = y + h, x2 = x + w;
    if (y2 > fb_height) y2 = fb_height;
    if (x2 > fb_width) x2 = fb_width;
    if (y2 <= y || x2 <= x) return;

    struct fb_dmg r; r.x0 = x; r.y0 = y; r.x1 = x2; r.y1 = y2;

    // Absorb every rectangle worth absorbing, repeatedly: swallowing one can
    // make the union worth merging with another.
    for (;;) {
        int hit = -1;
        for (int i = 0; i < dmg_n; i++) {
            if (!dmg_worth_merging(&dmg[i], &r)) continue;
            if (dmg[i].x0 < r.x0) r.x0 = dmg[i].x0;
            if (dmg[i].y0 < r.y0) r.y0 = dmg[i].y0;
            if (dmg[i].x1 > r.x1) r.x1 = dmg[i].x1;
            if (dmg[i].y1 > r.y1) r.y1 = dmg[i].y1;
            hit = i;
            break;
        }
        if (hit < 0) break;
        dmg[hit] = dmg[--dmg_n];        // the union takes its place
    }
    if (dmg_n < FB_DMG_MAX) { dmg[dmg_n++] = r; return; }

    // Full: fold into whichever rectangle grows least by taking it.
    int best = 0;
    uint64_t bestgrow = ~0ull;
    for (int i = 0; i < dmg_n; i++) {
        uint32_t ux0 = dmg[i].x0 < r.x0 ? dmg[i].x0 : r.x0;
        uint32_t uy0 = dmg[i].y0 < r.y0 ? dmg[i].y0 : r.y0;
        uint32_t ux1 = dmg[i].x1 > r.x1 ? dmg[i].x1 : r.x1;
        uint32_t uy1 = dmg[i].y1 > r.y1 ? dmg[i].y1 : r.y1;
        uint64_t grow = (uint64_t)(ux1 - ux0) * (uy1 - uy0) -
                        (uint64_t)(dmg[i].x1 - dmg[i].x0) * (dmg[i].y1 - dmg[i].y0);
        if (grow < bestgrow) { bestgrow = grow; best = i; }
    }
    if (dmg[best].x0 > r.x0) dmg[best].x0 = r.x0;
    if (dmg[best].y0 > r.y0) dmg[best].y0 = r.y0;
    if (dmg[best].x1 < r.x1) dmg[best].x1 = r.x1;
    if (dmg[best].y1 < r.y1) dmg[best].y1 = r.y1;
}

void fb_mark_rows(uint32_t y, uint32_t h) {
    fb_mark_rect(0, y, fb_width, h);
}

int fb_console_wrote(void) {
    int v = console_wrote;
    console_wrote = 0;
    return v;
}
static uint32_t fb_cols = 0;
static uint32_t fb_rows = 0;
static uint32_t cursor_col = 0;
static uint32_t cursor_row = 0;
static uint32_t color_fg = 0x00FFFFFF;
static uint32_t color_bg = 0x00000000;

#define DEFAULT_FG 0x00FFFFFF
#define DEFAULT_BG 0x00000000

// ANSI escape parser state (subset for TUI programs)
static int   ansi_state = 0;      // 0=normal, 1=saw ESC, 2=in CSI
static int   ansi_params[6];
static int   ansi_nparam = 0;

// Basic 8-color ANSI palette (indices 0-7), plus bright variants applied by +8.
static const uint32_t ansi_palette[16] = {
    0x00000000, 0x00CC0000, 0x0000CC00, 0x00CCCC00,
    0x000000CC, 0x00CC00CC, 0x0000CCCC, 0x00CCCCCC,
    0x00666666, 0x00FF4444, 0x0044FF44, 0x00FFFF44,
    0x004444FF, 0x00FF44FF, 0x0044FFFF, 0x00FFFFFF,
};

void fb_init(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp) {
    if (bpp != 32) {
        fb_mem = NULL;
        return;
    }
    fb_mem = (volatile uint8_t *)(uintptr_t)addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    fb_cols = fb_width / GLYPH_W;
    fb_rows = fb_height / GLYPH_H;
    cursor_col = 0;
    cursor_row = 0;
    draw_target = fb_mem;   // boot: draw straight to the screen
}

// Switch drawing to an off-screen back buffer (double buffering). The static
// store covers up to 1920x1080; on real hardware the firmware may hand us a
// bigger mode (higher resolution, or a scanline pitch padded past width*4), so
// for anything larger we allocate a back buffer sized to the ACTUAL
// framebuffer. Double buffering must never silently switch off -- direct
// rendering to slow video memory is exactly what makes the screen flicker.
void fb_enable_backbuffer(void) {
    if (!fb_mem) return;
    uint64_t need = (uint64_t)fb_pitch * fb_height;
    if (need <= FB_BACK_MAX) {
        fb_back = fb_back_store;
    } else {
        uint8_t *dyn = (uint8_t *)kmalloc((size_t)need);
        if (!dyn) { fb_back = NULL; return; }   // no memory: stay direct (last resort)
        fb_back = dyn;
    }
    draw_target = fb_back;

    // The shadow is an optimisation, not a requirement: without it fb_present
    // just blits the whole frame as before.
    fb_shadow = (uint8_t *)kmalloc((size_t)need);
    if (fb_shadow) {
        // Must not start out matching the back buffer, or the first frame would
        // be considered "already on screen" and never pushed.
        for (uint64_t i = 0; i < need; i++) fb_shadow[i] = 0xAA;
    }
}

// 1 if double buffering is active (drawing off-screen). For boot diagnostics.
int fb_backbuffer_active(void) { return fb_back != NULL && draw_target == fb_back; }

// Blit the back buffer to the screen in one pass. Called once per rendered
// frame by the WM; nothing intermediate is ever visible, so no flicker.
static uint32_t present_bytes, present_rows, present_rects;
static uint32_t present_frames;
uint32_t fb_last_bytes(void) { return present_bytes; }
uint32_t fb_last_rows(void)  { return present_rows; }
uint32_t fb_last_rects(void) { return present_rects; }

// Frames that actually reached the screen. Not frames drawn, not frames asked
// for -- presents that pushed at least one pixel across the bus. This is the
// only counter that answers "how many frames does it put out".
uint32_t fb_frames_pushed(void) { return present_frames; }

// Drop the accumulated damage without copying it. Only the benchmark uses
// this: it is how "what did the drawing cost" gets separated from "what did
// the screen cost".
void fb_present_discard(void) {
    dmg_n = 0;
}

void fb_present(void) {
    if (!fb_back || draw_target != fb_back) return;

    int n = dmg_n;
    dmg_n = 0;
    present_bytes = 0;
    present_rows = 0;
    present_rects = (uint32_t)n;
    if (!n) return;                          // nothing was drawn this frame

    // Copy the changed rectangles, and nothing else.
    //
    // The version before this one copied whole scanlines and used a shadow
    // buffer to decide which words inside them were worth writing. Measured on
    // a frame that touched one window, that comparison read about ten megabytes
    // of RAM to avoid writing ninety kilobytes: six million cycles spent to
    // save a few thousand. Knowing what changed makes the question unnecessary.
    for (int i = 0; i < n; i++) {
        uint32_t x0 = dmg[i].x0, x1 = dmg[i].x1;
        uint32_t y0 = dmg[i].y0, y1 = dmg[i].y1;
        uint32_t npix = x1 - x0;

        for (uint32_t y = y0; y < y1; y++) {
            size_t off = (size_t)y * fb_pitch + (size_t)x0 * 4;
            const uint32_t *b = (const uint32_t *)(fb_back + off);
            volatile uint32_t *d = (volatile uint32_t *)(fb_mem + off);

            uint32_t lo = 0, hi = npix;
            if (fb_shadow) {
                // Narrow the row to the part that really differs. Rows that
                // match at both ends are common: a repainted desktop mostly
                // reproduces itself, and an identical row costs nothing here.
                uint32_t *sh = (uint32_t *)(fb_shadow + off);
                while (lo < hi && b[lo] == sh[lo]) lo++;
                while (hi > lo && b[hi - 1] == sh[hi - 1]) hi--;
                if (lo == hi) continue;                 // row already on screen
                for (uint32_t k = lo; k < hi; k++) sh[k] = b[k];
            }

            present_rows++;
            present_bytes += (hi - lo) * 4;

            // 64-bit steps where the span allows; the ends a word at a time.
            uint32_t k = lo;
            if ((k & 1) && k < hi) { d[k] = b[k]; k++; }
            uint32_t pairs = (hi - k) / 2;
            const uint64_t *s8 = (const uint64_t *)(b + k);
            volatile uint64_t *d8 = (volatile uint64_t *)(d + k);
            for (uint32_t j = 0; j < pairs; j++) d8[j] = s8[j];
            for (uint32_t t = k + pairs * 2; t < hi; t++) d[t] = b[t];
        }
    }
    if (present_bytes) present_frames++;
}

int fb_available(void) {
    return fb_mem != NULL;
}

void fb_set_color(uint32_t fg, uint32_t bg) {
    color_fg = fg;
    color_bg = bg;
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    volatile uint32_t *p = (volatile uint32_t *)(draw_target + y * fb_pitch + x * 4);
    *p = color;
}

static void scroll(void) {
    fb_mark_rows(0, fb_height);
    if (draw_target == fb_back) console_wrote = 1;
    for (uint32_t y = GLYPH_H; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            volatile uint32_t *src = (volatile uint32_t *)(draw_target + y * fb_pitch + x * 4);
            volatile uint32_t *dst = (volatile uint32_t *)(draw_target + (y - GLYPH_H) * fb_pitch + x * 4);
            *dst = *src;
        }
    }
    for (uint32_t y = fb_height - GLYPH_H; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            put_pixel(x, y, color_bg);
        }
    }
}

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    const uint8_t *glyph = font8x8_basic[(uint8_t)c];
    uint32_t base_x = col * GLYPH_W;
    uint32_t base_y = row * GLYPH_H;
    fb_mark_rows(base_y, GLYPH_H);
    if (draw_target == fb_back) console_wrote = 1;
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            uint32_t color = (bits & (1 << (GLYPH_W - 1 - gx))) ? color_fg : color_bg;
            put_pixel(base_x + gx, base_y + gy, color);
        }
    }
}

static void newline(void) {
    cursor_col = 0;
    if (cursor_row + 1 >= fb_rows) {
        scroll();
    } else {
        cursor_row++;
    }
}

static void erase_to_eol(void) {
    for (uint32_t col = cursor_col; col < fb_cols; col++) {
        draw_glyph(col, cursor_row, ' ');
    }
}

static void set_sgr(int p) {
    switch (p) {
        case 0:  color_fg = DEFAULT_FG; color_bg = DEFAULT_BG; break; // reset
        case 7: { uint32_t t = color_fg; color_fg = color_bg; color_bg = t; break; } // reverse
        default:
            if (p >= 30 && p <= 37)        color_fg = ansi_palette[p - 30];
            else if (p >= 90 && p <= 97)   color_fg = ansi_palette[(p - 90) + 8];
            else if (p >= 40 && p <= 47)   color_bg = ansi_palette[p - 40];
            else if (p >= 100 && p <= 107) color_bg = ansi_palette[(p - 100) + 8];
            break;
    }
}

// Execute a CSI sequence: ESC '[' params <final>.
static void ansi_dispatch(char final) {
    int p0 = (ansi_nparam > 0) ? ansi_params[0] : 0;
    int p1 = (ansi_nparam > 1) ? ansi_params[1] : 0;
    switch (final) {
        case 'H': case 'f': { // cursor to row;col (1-based; missing => 1)
            uint32_t row = (p0 > 0) ? (uint32_t)(p0 - 1) : 0;
            uint32_t col = (p1 > 0) ? (uint32_t)(p1 - 1) : 0;
            if (row >= fb_rows) row = fb_rows - 1;
            if (col >= fb_cols) col = fb_cols - 1;
            cursor_row = row; cursor_col = col;
            break;
        }
        case 'J': // 2 = clear whole screen and home
            if (p0 == 2) fb_console_reset();
            break;
        case 'K': // erase from cursor to end of line
            erase_to_eol();
            break;
        case 'm': // SGR (colors / reverse)
            if (ansi_nparam == 0) { set_sgr(0); }
            else for (int i = 0; i < ansi_nparam; i++) set_sgr(ansi_params[i]);
            break;
        case 'A': { uint32_t n = (p0 > 0) ? (uint32_t)p0 : 1; cursor_row = (cursor_row > n) ? cursor_row - n : 0; break; }
        case 'B': { uint32_t n = (p0 > 0) ? (uint32_t)p0 : 1; cursor_row += n; if (cursor_row >= fb_rows) cursor_row = fb_rows - 1; break; }
        case 'C': { uint32_t n = (p0 > 0) ? (uint32_t)p0 : 1; cursor_col += n; if (cursor_col >= fb_cols) cursor_col = fb_cols - 1; break; }
        case 'D': { uint32_t n = (p0 > 0) ? (uint32_t)p0 : 1; cursor_col = (cursor_col > n) ? cursor_col - n : 0; break; }
        default: break;
    }
}

void fb_putc(char c) {
    if (!fb_mem) return;

    // --- ANSI escape state machine ---
    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_nparam = 0;
            ansi_params[0] = 0;
        } else {
            ansi_state = 0; // unsupported escape; drop
        }
        return;
    }
    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (ansi_nparam == 0) ansi_nparam = 1;
            ansi_params[ansi_nparam - 1] = ansi_params[ansi_nparam - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (ansi_nparam < 6) { ansi_params[ansi_nparam] = 0; ansi_nparam++; }
            return;
        }
        ansi_dispatch(c);
        ansi_state = 0;
        return;
    }
    if (c == 0x1B) { // ESC
        ansi_state = 1;
        return;
    }

    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        cursor_col = 0;
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) cursor_col--;
        return;
    }

    draw_glyph(cursor_col, cursor_row, c);
    cursor_col++;
    if (cursor_col >= fb_cols) {
        newline();
    }
}

void fb_write(const char *s) {
    while (*s) {
        fb_putc(*s++);
    }
}

// --- compositor primitives ---

void fb_console_size(uint32_t *cols, uint32_t *rows) {
    if (cols) *cols = fb_cols;
    if (rows) *rows = fb_rows;
}

uint32_t fb_get_width(void) {
    return fb_width;
}

volatile uint8_t *fb_get_base(void) {
    return draw_target;   // WM/font draw into the active target (back buffer)
}

uint32_t fb_get_pitch(void) {
    return fb_pitch;
}

uint32_t fb_get_height(void) {
    return fb_height;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_mem) return;
    if (x >= fb_width || y >= fb_height) return;
    uint32_t x1 = x + w;
    uint32_t y1 = y + h;
    if (x1 > fb_width) x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;
    fb_mark_rect(x, y, x1 - x, y1 - y);

    uint32_t n = x1 - x;
    if (draw_target != fb_mem) {
        // Into the back buffer: plain memory, so write it like memory. Two
        // pixels per store, and nothing to stop the compiler widening that
        // further.
        uint64_t pair = ((uint64_t)color << 32) | color;
        for (uint32_t py = y; py < y1; py++) {
            uint8_t *row = (uint8_t *)draw_target + (size_t)py * fb_pitch + (size_t)x * 4;
            uint32_t i = 0;
            if (((uintptr_t)row & 7) && n) {          // odd start: one pixel
                *(uint32_t *)row = color;
                row += 4;
                i = 1;
            }
            uint64_t *p8 = (uint64_t *)row;
            uint32_t pairs = (n - i) / 2;
            for (uint32_t k = 0; k < pairs; k++) p8[k] = pair;
            if ((n - i) & 1) *(uint32_t *)(row + pairs * 8) = color;
        }
        return;
    }

    for (uint32_t py = y; py < y1; py++)
        for (uint32_t px = x; px < x1; px++)
            put_pixel(px, py, color);
}

void fb_draw_glyph_at(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg) {
    if (!fb_mem) return;
    fb_mark_rect(px, py, GLYPH_W, GLYPH_H);
    const uint8_t *glyph = font8x8_basic[(uint8_t)c];
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        if (py + gy >= fb_height) break;
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
            if (px + gx >= fb_width) continue;
            uint32_t color = (bits & (1 << (GLYPH_W - 1 - gx))) ? fg : bg;
            put_pixel(px + gx, py + gy, color);
        }
    }
}

void fb_console_reset(void) {
    if (!fb_mem) return;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            put_pixel(x, y, color_bg);
        }
    }
    cursor_col = 0;
    cursor_row = 0;
}
