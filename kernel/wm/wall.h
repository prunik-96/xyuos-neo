#ifndef WM_WALL_H
#define WM_WALL_H

#include <stdint.h>

/* The default wallpaper for the light (dark = 0) or dark desktop, painted
 * into w x h pixels. */
void wall_paint(uint32_t *px, int w, int h, int dark);

/* What glass shows: `src` blurred and its colour lifted, into `dst`. */
void wall_frost(const uint32_t *src, uint32_t *dst, int w, int h, int dark);

#endif
