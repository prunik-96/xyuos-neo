#ifndef HID_H
#define HID_H

#include <stdint.h>

// Where a mouse's report keeps its buttons, X, Y and wheel -- read from the
// report descriptor the mouse itself hands over, not assumed.
//
// The boot protocol promises a fixed 3-byte layout (buttons, dx, dy) to any
// host that asks for it, and plenty of mice keep that promise. Not all: a
// Logitech LIGHTSPEED receiver answers SET_PROTOCOL(boot) and goes on sending
// its own reports -- 16 buttons in two bytes, X and Y in 16 bits each -- so a
// driver that reads byte 1 as X gets the upper half of the buttons, always 0,
// and byte 2 as Y gets the low byte of X: the pointer moves up and down when
// the mouse moves left and right. Reading the descriptor is what an operating
// system is meant to do, and it works for every mouse.
typedef struct {
    uint8_t id;                  // report ID the fields are in; 0 if none
    int16_t btn[3];              // bit of left, right, middle; -1 if absent
    int16_t x, y, wheel;         // first bit of each; -1 if absent
    uint8_t xs, ys, ws;          // their sizes in bits
    uint8_t xsig, ysig, wsig;    // whether they are signed
} hid_mouse_fmt;

// Fill `f` from a report descriptor. 1 if it describes relative X and Y.
int hid_parse_mouse(const uint8_t *desc, int len, hid_mouse_fmt *f);

// The boot protocol's fixed layout, for a mouse whose descriptor could not be
// read or says nothing usable.
void hid_mouse_boot(hid_mouse_fmt *f);

// Read one report of `len` bytes. 0 if it is not one of this format's (a
// report with another ID), 1 with the fields filled in otherwise. `buttons`
// has left, right and middle in bits 0, 1, 2.
int hid_mouse_read(const hid_mouse_fmt *f, const uint8_t *r, int len,
                   int *buttons, int *dx, int *dy, int *wheel);

#endif
