#ifndef WM_UI_H
#define WM_UI_H

#include <stdint.h>
#include "../gfx/uifont.h"

/* What the desktop is drawn with: rounded rectangles of glass and gradient,
 * soft shadows, proportional text, pictures with an alpha channel.
 *
 * Integers only, every one of these: the compositor paints inside system
 * calls, where the floating-point registers are the calling program's.
 *
 * Everything draws into the back buffer, inside the current clip rectangle,
 * and reports what it touched to fb_mark_rect so it reaches the screen. */

/* --- colour ---------------------------------------------------------------- */

/* a and b mixed, t of b in 256ths. */
static inline uint32_t ui_mix(uint32_t a, uint32_t b, int t) {
    uint32_t u = (uint32_t)t, v = 256 - u;
    uint32_t rb = (((a & 0xFF00FF) * v + (b & 0xFF00FF) * u) >> 8) & 0xFF00FF;
    uint32_t g  = (((a & 0x00FF00) * v + (b & 0x00FF00) * u) >> 8) & 0x00FF00;
    return rb | g;
}

/* --- clipping --------------------------------------------------------------- */

void ui_clip(int x, int y, int w, int h);   /* intersect with the screen */
void ui_noclip(void);                       /* the whole screen          */
void ui_clip_get(int r[4]);
void ui_clip_set(const int r[4]);

/* --- flat things -------------------------------------------------------------- */

void ui_fill(int x, int y, int w, int h, uint32_t rgb);
void ui_blend(int x, int y, int w, int h, uint32_t rgb, int alpha);

/* --- rounded rectangles ----------------------------------------------------------
 *
 * Which corners are rounded: */
#define UI_TL 1
#define UI_TR 2
#define UI_BL 4
#define UI_BR 8
#define UI_ALL 15
#define UI_TOP (UI_TL | UI_TR)

/* What fills one. */
enum { UI_SOLID, UI_GRAD, UI_GLASS };
typedef struct {
    int      kind;
    uint32_t c0, c1;      /* SOLID: c0. GRAD: top, bottom. GLASS: tint c0  */
    int      a0, a1;      /* SOLID: a0. GRAD: alpha at top, bottom.         */
                          /* GLASS: how much tint over the frosted backdrop */
    int      gy0, gy1;    /* GRAD: the rows the ramp spans (0,0: the shape) */
} ui_mat;

void ui_rrect(int x, int y, int w, int h, int r, int corners, const ui_mat *m);
/* Rounder at the top than at the bottom, like a window. */
void ui_rrect2(int x, int y, int w, int h, int rt, int rb, const ui_mat *m);

/* A one-pixel line along the inside of the same shapes. */
void ui_rrect_line(int x, int y, int w, int h, int r, int corners,
                   uint32_t rgb, int alpha);
void ui_rrect2_line(int x, int y, int w, int h, int rt, int rb,
                    uint32_t rgb, int alpha);

/* Any shape librast can draw, onto the screen inside the clip. */
#include "../../librast/include/rast.h"
void ui_rast_fill(const rast_path *p, const rast_paint *paint);
void ui_rast_stroke(const rast_path *p, const rast_stroke *s, const rast_paint *paint);

/* Shorthands. */
void ui_round_fill(int x, int y, int w, int h, int r, uint32_t rgb, int alpha);
void ui_round_grad(int x, int y, int w, int h, int r, int corners,
                   uint32_t top, int atop, uint32_t bot, int abot);

/* The shadow a rounded rectangle casts: `size` pixels of blur, darkest
 * `alpha`, pushed `dy` down. Drawn only OUTSIDE the rectangle -- whatever
 * casts it covers the inside -- and never outside `limit` (x, y, w, h). */
void ui_shadow(int x, int y, int w, int h, int r, int size, int alpha, int dy,
               const int limit[4]);

/* --- the frosted backdrop ------------------------------------------------------
 *
 * Glass shows the wallpaper behind it, blurred -- only the wallpaper, never
 * the windows, so a pane of glass looks the same wherever it is drawn and
 * drawing it twice changes nothing. */
void ui_set_backdrop(const uint32_t *px, int w, int h);

/* --- pictures --------------------------------------------------------------------
 *
 * 0xAARRGGBB, premultiplied, w x h, drawn at `alpha` (255: as they are). */
void ui_image(int x, int y, const uint32_t *px, int w, int h, int alpha);

/* --- text ------------------------------------------------------------------------
 *
 * y is the top of the line box; the baseline is ui_faces[face].ascent below.
 * Strings are UTF-8. */
int  ui_text(int x, int y, const char *s, int face, uint32_t rgb);
int  ui_text_w(const char *s, int face);
int  ui_line_h(int face);
/* At most `maxw` wide, an ellipsis replacing what does not fit. */
int  ui_text_fit(int x, int y, int maxw, const char *s, int face, uint32_t rgb);
/* With a soft halo of `glow` around the letters -- text on glass, Windows 7
 * style, stays readable whatever the wallpaper does behind it. */
int  ui_text_glow(int x, int y, const char *s, int face, uint32_t rgb,
                  uint32_t glow, int glow_alpha);

/* --- whole images in memory ------------------------------------------------------- */

/* Box blur, three passes of it: close enough to a Gaussian of radius r. */
void ui_blur(uint32_t *px, int w, int h, int r);

#endif
