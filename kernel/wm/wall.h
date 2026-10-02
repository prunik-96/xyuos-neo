#ifndef WM_WALL_H
#define WM_WALL_H

#include <stdint.h>

/* The default wallpaper for the light (dark = 0) or dark desktop, painted
 * into w x h pixels. */
void wall_paint(uint32_t *px, int w, int h, int dark);

/* The calm wallpapers of the new desktop: soft light and colour, nothing
 * sharp. */
#define WALL_DAWN   0           /* peach into lilac, warm soft lights      */
#define WALL_LAGOON 1           /* teal water, slow bands of light          */
#define WALL_MIST   2           /* blue hills fading into fog               */
#define WALL_STYLES 3
void wall_soft(uint32_t *px, int w, int h, int style);

/* What glass shows: `src` blurred and its colour lifted, into `dst`. */
void wall_frost(const uint32_t *src, uint32_t *dst, int w, int h, int dark);

#endif
