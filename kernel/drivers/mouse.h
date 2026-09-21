#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Pointer state and event queue. Deliberately driver-agnostic: the USB HID
// driver (xhci.c) feeds it raw boot-protocol reports, and the window manager
// reads cooked events. A PS/2 mouse could feed the same entry point.

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

struct mouse_event {
    int32_t x, y;        // absolute position, already clamped to the screen
    uint8_t buttons;     // MOUSE_* bitmask: what is held right now
    uint8_t pressed;     // buttons that went DOWN in this event
    uint8_t released;    // buttons that came UP in this event
    int8_t  wheel;       // notches, positive = away from the user
};

// Bound the cursor to a screen of this size and place it in the middle.
void mouse_init(uint32_t screen_w, uint32_t screen_h);
void mouse_set_bounds(uint32_t screen_w, uint32_t screen_h);

// 1 once a physical mouse has been found and configured.
int  mouse_present(void);
void mouse_set_present(int yes);

// Feed one HID boot-protocol report: button bitmap plus relative motion.
void mouse_inject(uint8_t buttons, int dx, int dy, int wheel);

// Non-blocking; 1 if an event was dequeued.
int  mouse_poll_event(struct mouse_event *out);

// Where the pointer is right now (for drawing the cursor).
// Pointer speed as a percentage: 100 is the hardware's own scale, 50 is half
// as far per unit of motion, 200 twice. Clamped to something usable.
void mouse_set_speed(int percent);
int  mouse_speed(void);

void mouse_position(int32_t *x, int32_t *y);
uint8_t mouse_buttons(void);

#endif
