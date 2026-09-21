#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>

// The icon set, baked at build time into one blob (/icons.bin) and loaded
// whole at start-up. Two sizes only -- 32 for the start menu, 16 for the
// taskbar, window titles and lists -- because those are the sizes the
// interface uses and scaling at draw time would only lose detail.
#define ICON_BIG   32
#define ICON_SMALL 16

int  icons_load(void);            // read /icons.bin; 1 if anything was found

// The pixels for `name`, as 0xAARRGGBB, or NULL when there is no such icon.
// A missing icon is not an error: callers draw what they drew before.
const uint32_t *icon_get(const char *name, int size);

// Blend an icon into the current draw target at (x, y). Returns 0 if there is
// no such icon, so the caller can fall back.
int  icon_draw(int x, int y, const char *name, int size);

#endif
