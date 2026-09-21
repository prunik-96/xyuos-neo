/* The toolbar, the tabs and the message line.
 *
 * Drawing and hit-testing are the same pass: each control records where it
 * ended up, and a click is answered by looking through that list. Keeping
 * the two together is what stops a button from being somewhere other than
 * where it looks.
 */
#ifndef XYUOS_NS_CHROME_H
#define XYUOS_NS_CHROME_H

#include "xy_front.h"

enum {
    HIT_NONE = 0,
    HIT_BACK, HIT_FORWARD, HIT_RELOAD, HIT_STOP, HIT_HOME,
    HIT_ADDRESS,
    HIT_TAB, HIT_TAB_CLOSE, HIT_TAB_NEW,
};

#define XY_HIT_MAX 32

struct hit { int kind, arg, x, y, w, h; };

extern struct hit xy_hits[XY_HIT_MAX];
extern int xy_nhits;

void xy_chrome_draw(gui_edit_t *address, int address_focused);
void xy_status_draw(void);

/* What is at this point, and which tab if it is one. */
int  xy_chrome_hit(int x, int y, int *arg);

#endif
