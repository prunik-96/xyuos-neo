#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

// The boot screen: the logo on a calm field and a light running along a thin
// line while the machine comes up. The kernel's log goes on being kept (and
// sent down the serial line) but is not drawn -- until a key is pressed, when
// it is shown from the start.

// Put it up. After the heap: it draws into memory and copies to the screen.
// The text console goes quiet from here on.
void splash_start(void);

// The disk is mounted: the desktop's font is there now, so the name and the
// hint can be drawn.
void splash_fonts(void);

// From the timer interrupt: the next step of the running light, and a look
// at the keyboard.
void splash_tick(void);

// From the log's own output path: if a key asked for the log, show it now
// (here, outside an interrupt, where drawing text is safe).
void splash_poll(void);

// The desktop takes over. Returns the boot screen's picture, for the desktop
// to fade from (the caller frees it with kfree), or 0 if there is none --
// no screen, or the log is showing.
uint32_t *splash_end(void);

#endif
