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
// Work spread over the cores: fn(y0, y1, share, ctx) for bands of the rows
// y0..y1, each band on whichever core is free (share says which one, for a
// scratch buffer of its own). Small jobs, under ~20000 pixels by px_per_row,
// are simply done here. A band must touch nothing but its own rows: no
// allocating, no reporting to the screen, no calling ui_bands again.
typedef void (*ui_band_fn)(int y0, int y1, int share, void *ctx);
void ui_bands(ui_band_fn fn, void *ctx, int y0, int y1, int px_per_row);
int  ui_band_count(void);        // how many shares there can be at most

static inline uint32_t ui_mix(uint32_t a, uint32_t b, int t) {
    uint32_t u = (uint32_t)t, v = 256 - u;
    uint32_t rb = (((a & 0xFF00FF) * v + (b & 0xFF00FF) * u) >> 8) & 0xFF00FF;
    uint32_t g  = (((a & 0x00FF00) * v + (b & 0x00FF00) * u) >> 8) & 0x00FF00;
    return rb | g;
}

/* --- where it goes ------------------------------------------------------------
 *
 * Normally the screen's back buffer. ui_target points everything at a
 * picture in memory instead (w x h, rows packed), until ui_target_screen.
 * Both reset the clip to the whole of the new target. */
void ui_target(uint32_t *px, int w, int h);
void ui_target_screen(void);

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

/* Coloured glass over whatever is already drawn under it -- windows
 * included: that region is blurred by `blur` pixels, coloured by `tint` at
 * `tint_a` (0..255), given a fine grain and a soft light along its edge, and
 * laid down as a rounded rectangle of radius r. */
void ui_glass_live(int x, int y, int w, int h, int r, uint32_t tint, int tint_a, int blur);

/* --- glass that remembers what is under it -----------------------------------
 *
 * Glass shows what is behind it, blurred -- and on a screen that is only ever
 * partly repainted, "behind" is not what the back buffer holds where the
 * glass is: that is last frame's glass. So each pane of glass keeps its own
 * copy of what lies under it, a quarter the size each way (it is going to be
 * blurred, detail is wasted), and refreshes it only where something under it
 * was actually repainted this frame -- taken from the back buffer at the
 * moment that is still the truth, before the glass goes on top. */
typedef struct {
    int ux, uy, uw, uh;        /* the screen area remembered, 4-aligned     */
    int sw, sh;                /* the same, in quarter pixels               */
    uint32_t *small;           /* what is under, each 4x4 averaged          */
    uint32_t *soft;            /* the same, blurred                         */
    int cap;                   /* pixels allocated in each                  */
    int have, soft_ok;
} ui_glass;

/* The glass will cover (x, y, w, h); its blur reaches `pad` beyond that.
 * Moving it forgets what it knew. */
void ui_glass_place(ui_glass *g, int x, int y, int w, int h, int pad);
void ui_glass_forget(ui_glass *g);
void ui_glass_release(ui_glass *g);
/* Refresh from the current target what lies inside any of the n damaged
 * rectangles (x0, y0, x1, y1) -- or all of it, if it knows nothing yet. */
void ui_glass_take(ui_glass *g, const int (*dmg)[4], int n);
/* Glass in the shape of a rounded rectangle, or of one with a rounded
 * rectangle cut out of it (a window's frame round its body: bw = 0 for
 * none). Tinted `tint` at `tint_a`. */
void ui_glass_draw(ui_glass *g, int x, int y, int w, int h, int r,
                   int bx, int by, int bw, int bh, int br, int bcorners,
                   uint32_t tint, int tint_a);
/* The light along glass's edge: a sheen down from the top, a fine line. */
void ui_glass_rim(int x, int y, int w, int h, int r);

/* A colour for the system to wear, taken from a picture: the average of its
 * most colourful pixels, brought to a lightness that reads as an accent. */
uint32_t ui_accent_from(const uint32_t *px, int w, int h);

/* --- pictures --------------------------------------------------------------------
 *
 * 0xAARRGGBB, premultiplied, w x h, drawn at `alpha` (255: as they are). */
void ui_image(int x, int y, const uint32_t *px, int w, int h, int alpha);

/* An icon of the icon set (gfx/icons.h) at one of its sizes; 0 if there is
 * none, for the caller to draw something of its own. */
int  ui_icon(int x, int y, const char *name, int size);

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
