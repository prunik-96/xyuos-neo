#ifndef WM_CURSOR_H
#define WM_CURSOR_H

#include <stdint.h>

/* The pointer's pictures: anti-aliased, with a soft shadow, drawn once at
 * start-up by librast. Each is CUR_SZ square, premultiplied 0xAARRGGBB,
 * with a hot spot -- the pixel that is "where the pointer is". */

#define CUR_SZ     32
#define CUR_FRAMES 16          /* the spinning ones go round in this many */

enum {
    CUR_ARROW,
    CUR_EW,                    /* resize: left-right               */
    CUR_NS,                    /*         up-down                  */
    CUR_NWSE,                  /*         top-left / bottom-right  */
    CUR_NESW,                  /*         top-right / bottom-left  */
    CUR_BUSY,                  /* a ring going round               */
    CUR_APPSTART,              /* the arrow, with a small ring     */
    CUR_KINDS
};

void cursor_init(void);
const uint32_t *cursor_image(int kind, int frame, int *hx, int *hy);

#endif
