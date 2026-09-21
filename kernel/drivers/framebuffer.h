#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>

#define FB_GLYPH_W 8
#define FB_GLYPH_H 8

void fb_init(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp);
int  fb_available(void);

// --- single-stream console (boot log + standard shell via SYS_WRITE) ---
void fb_putc(char c);
void fb_write(const char *s);
void fb_set_color(uint32_t fg, uint32_t bg);
void fb_console_reset(void);   // clear screen to bg and home the console cursor
void fb_console_size(uint32_t *cols, uint32_t *rows); // character grid dimensions

// --- compositor primitives (in-kernel window manager) ---
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
volatile uint8_t *fb_get_base(void);   // active draw target (front or back buffer)
uint32_t fb_get_pitch(void);           // bytes per scanline

// Double buffering (flicker-free WM rendering).
// Report that scanlines [y, y+h) were drawn into. fb_present() only examines
// marked rows, so anything writing to the draw target directly (rather than
// through fb_fill_rect / fb_draw_glyph_at) must say so.
void fb_mark_rows(uint32_t y, uint32_t h);
// Record that [x, x+w) x [y, y+h) changed. Everything that draws must say so,
// or its pixels will not reach the screen.
void fb_mark_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Bytes pushed to video memory by the last fb_present(), the rows it copied,
// and how many separate rectangles it needed. Purely for measurement.
uint32_t fb_last_bytes(void);
uint32_t fb_last_rows(void);
uint32_t fb_last_rects(void);

// Frames that actually reached the screen since boot -- presents that pushed
// at least one pixel. The honest denominator for a frame rate.
uint32_t fb_frames_pushed(void);

// 1 (once) if kernel logging has drawn into the WM's back buffer since the last
// call, so the compositor knows to repaint the desktop over it.
int  fb_console_wrote(void);

void fb_enable_backbuffer(void);       // draw off-screen from now on
int  fb_backbuffer_active(void);       // 1 if double buffering is on
void fb_present(void);                 // blit back buffer -> screen
void fb_present_discard(void);         // drop the damage unpainted (benchmark)
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_glyph_at(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg);

#endif
