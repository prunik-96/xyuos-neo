// The boot screen (see splash.h).
//
// Everything is drawn into a picture of the whole screen in memory and only
// copied out: the screen at this point is the firmware's framebuffer itself,
// with no back buffer in front of it, and reading video memory -- which
// blending has to -- is slow enough to see.

#include "splash.h"
#include "ui.h"
#include "glyphs.h"
#include "../gfx/uifont.h"
#include "../drivers/framebuffer.h"
#include "../drivers/keyboard.h"
#include "../arch/x86_64/pit.h"
#include "../mm/heap.h"
#include "../fs/vfs.h"
#include "../kernel/kio.h"

#define TOP     0x000C2E3A        // the field: deep lagoon, darker towards the bottom
#define BOTTOM  0x00061920
#define GLOW    0x002A9DAE        // the light behind the logo
#define INK     0x00EAF7F9
#define BAR_W   260               // the line the light runs along
#define BAR_H   4
#define SEG_W   84                // the light itself
#define LAP_MS  1500              // one run, left to right

static uint32_t *img;             // the whole screen, drawn here first
static uint32_t *under;           // the line as it is with no light on it
static int W, H, cx, cy, R, bar_x, bar_y;
static volatile int running;      // the light is moving
static volatile int want_log;     // a key asked for the log
static int log_shown;

static void push(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    volatile uint8_t *base = fb_get_base();
    uint32_t pitch = fb_get_pitch();
    for (int j = 0; j < h; j++) {
        volatile uint32_t *d = (volatile uint32_t *)(base + (uint64_t)(y + j) * pitch) + x;
        const uint32_t *s = img + (uint64_t)(y + j) * W + x;
        for (int i = 0; i < w; i++) d[i] = s[i];
    }
}

// The field: a gradient, and a soft light round where the logo stands.
static void field(void) {
    int64_t gr = (int64_t)H * 46 / 100;
    int64_t gr2 = gr * gr;
    for (int y = 0; y < H; y++) {
        uint32_t base = ui_mix(TOP, BOTTOM, y * 256 / H);
        int64_t dy = y - cy;
        uint32_t *row = img + (uint64_t)y * W;
        for (int x = 0; x < W; x++) {
            int64_t dx = x - cx;
            int64_t d2 = dx * dx + dy * dy;
            uint32_t c = base;
            if (d2 < gr2) {
                int t = (int)(256 - d2 * 256 / gr2);
                t = t * t / 256;
                c = ui_mix(base, GLOW, t * 80 / 256);
            }
            row[x] = c;
        }
    }
}

// One step of the light: the line put back, the light drawn on it at where
// it is now, and only that strip copied to the screen.
static void bar_frame(uint64_t ms) {
    for (int j = 0; j < BAR_H; j++)
        for (int i = 0; i < BAR_W; i++)
            img[(uint64_t)(bar_y + j) * W + bar_x + i] = under[j * BAR_W + i];
    int t = (int)(ms % LAP_MS) * 256 / LAP_MS;
    int e = (t * t * (768 - 2 * t)) >> 16;          // smoothstep: it eases in and out
    int sx = bar_x - SEG_W + e * (BAR_W + SEG_W) / 256;
    ui_target(img, W, H);
    ui_clip(bar_x, bar_y, BAR_W, BAR_H);
    ui_round_fill(sx, bar_y, SEG_W, BAR_H, BAR_H / 2, 0x00BFF3F7, 235);
    ui_target_screen();
    push(bar_x, bar_y, BAR_W, BAR_H);
}

void splash_start(void) {
    if (!fb_available()) return;
    W = (int)fb_get_width();
    H = (int)fb_get_height();
    img = (uint32_t *)kmalloc((uint64_t)W * H * 4);
    under = (uint32_t *)kmalloc(BAR_W * BAR_H * 4);
    if (!img || !under) {
        if (img) kfree(img);
        if (under) kfree(under);
        img = under = 0;
        return;
    }
    cx = W / 2;
    cy = H * 42 / 100;
    R = H / 12;
    bar_x = cx - BAR_W / 2;
    bar_y = cy + R + 96;

    field();
    ui_target(img, W, H);
    glyph_logo(cx, cy, R, INK);
    ui_round_fill(bar_x, bar_y, BAR_W, BAR_H, BAR_H / 2, 0x00FFFFFF, 40);
    ui_target_screen();
    for (int j = 0; j < BAR_H; j++)
        for (int i = 0; i < BAR_W; i++)
            under[j * BAR_W + i] = img[(uint64_t)(bar_y + j) * W + bar_x + i];

    // From here the log is kept, not drawn: the boot screen is what shows.
    fb_console_mute(1);
    push(0, 0, W, H);
    running = 1;
}

// Whether the desktop was last left speaking English (/home/.desk, lang=1).
static int lang_english(void) {
    char b[128];
    int fd = vfs_open("/home/.desk");
    if (fd < 0) return 0;
    int n = vfs_read(fd, b, sizeof b - 1);
    vfs_close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    for (int i = 0; i + 6 < n; i++)
        if (b[i] == 'l' && b[i + 1] == 'a' && b[i + 2] == 'n' && b[i + 3] == 'g' && b[i + 4] == '=')
            return b[i + 5] == '1';
    return 0;
}

void splash_fonts(void) {
    if (!img || log_shown) return;
    uifont_init();                     // from the disk, now that there is one
    int en = lang_english();
    const char *name = "xyuOS Neo";
    const char *hint = en ? "Press any key to see the boot log"
                          : "Нажмите любую клавишу, чтобы увидеть журнал загрузки";
    // The light is drawn from the timer, with the same drawing state: hold
    // it off while the text goes in.
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    ui_target(img, W, H);
    int tw = ui_text_w(name, UI_F28);
    int ty = cy + R + 30;
    ui_text(cx - tw / 2, ty, name, UI_F28, 0x00F0FAFB);
    int hw = ui_text_w(hint, UI_F12);
    int hy = H - 56;
    ui_text(cx - hw / 2, hy, hint, UI_F12, ui_mix(BOTTOM, 0x00FFFFFF, 105));
    ui_target_screen();
    push(cx - tw / 2 - 4, ty - 4, tw + 8, ui_line_h(UI_F28) + 8);
    push(cx - hw / 2 - 4, hy - 4, hw + 8, ui_line_h(UI_F12) + 8);
    if (fl & 0x200) __asm__ volatile ("sti" ::: "memory");
}

void splash_tick(void) {
    if (!running) return;
    struct kbd_event ev;
    while (keyboard_poll_event(&ev)) want_log = 1;
    if (want_log) { running = 0; return; }       // shown from splash_poll
    static uint64_t last;
    uint64_t t = pit_get_ticks();
    if (t - last < 2) return;                    // fifty steps a second
    last = t;
    bar_frame(t * 10);
}

void splash_poll(void) {
    if (!want_log || log_shown || !img) return;
    // Not from inside an interrupt: drawing text uses the font renderer.
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0" : "=r"(fl));
    if (!(fl & 0x200)) return;
    log_shown = 1;
    running = 0;
    fb_console_mute(0);
    fb_console_reset();
    const char *b;
    uint32_t size, head;
    klog_snapshot(&b, &size, &head);
    for (uint32_t i = head > size ? head - size : 0; i < head; i++) fb_putc(b[i % size]);
}

uint32_t *splash_end(void) {
    running = 0;
    // The desktop owns the screen from here: the kernel's log is kept and sent
    // down the serial line, but no longer drawn over it.
    fb_console_mute(1);
    if (!img) return 0;
    if (under) { kfree(under); under = 0; }
    uint32_t *p = img;
    img = 0;
    if (log_shown || want_log) { kfree(p); return 0; }
    return p;
}
