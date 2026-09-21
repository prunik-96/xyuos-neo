#include "mouse.h"

// Events are produced in an interrupt-ish context (the timer tick drives the
// USB poll) and consumed by the compositor, so this is a single-producer,
// single-consumer ring. Dropping the oldest event on overflow is right for a
// pointer: stale motion is worthless, and the position is absolute anyway.
#define MOUSE_QUEUE 64

static struct mouse_event queue[MOUSE_QUEUE];
static uint32_t q_head, q_tail;

static int32_t  cur_x, cur_y;
static uint32_t bound_w = 640, bound_h = 480;
static uint8_t  cur_buttons;
static int      present;

void mouse_set_bounds(uint32_t screen_w, uint32_t screen_h) {
    if (screen_w) bound_w = screen_w;
    if (screen_h) bound_h = screen_h;
    if (cur_x > (int32_t)bound_w - 1) cur_x = (int32_t)bound_w - 1;
    if (cur_y > (int32_t)bound_h - 1) cur_y = (int32_t)bound_h - 1;
}

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    q_head = q_tail = 0;
    cur_buttons = 0;
    mouse_set_bounds(screen_w, screen_h);
    cur_x = (int32_t)bound_w / 2;
    cur_y = (int32_t)bound_h / 2;
}

int  mouse_present(void)          { return present; }
void mouse_set_present(int yes)   { present = yes ? 1 : 0; }
uint8_t mouse_buttons(void)       { return cur_buttons; }

void mouse_position(int32_t *x, int32_t *y) {
    if (x) *x = cur_x;
    if (y) *y = cur_y;
}

static int speed_pct = 100;
static int rem_x, rem_y;      /* the fraction the scaling threw away */

void mouse_set_speed(int percent) {
    if (percent < 25) percent = 25;
    if (percent > 400) percent = 400;
    speed_pct = percent;
    rem_x = rem_y = 0;
}

int mouse_speed(void) { return speed_pct; }

void mouse_inject(uint8_t buttons, int dx, int dy, int wheel) {
    // HID reports motion relative, with +Y pointing down -- the same direction
    // as screen coordinates, so it adds directly.
    //
    // The scaling keeps its remainder. Rounding each report on its own would
    // throw away most of a slow drag: at 50%, every single-unit report becomes
    // zero and the pointer simply refuses to move.
    if (speed_pct != 100) {
        int nx = dx * speed_pct + rem_x, ny = dy * speed_pct + rem_y;
        rem_x = nx % 100; rem_y = ny % 100;
        dx = nx / 100;    dy = ny / 100;
    }
    cur_x += dx;
    cur_y += dy;
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_x > (int32_t)bound_w - 1) cur_x = (int32_t)bound_w - 1;
    if (cur_y > (int32_t)bound_h - 1) cur_y = (int32_t)bound_h - 1;

    uint8_t changed  = (uint8_t)(buttons ^ cur_buttons);
    uint8_t pressed  = (uint8_t)(changed & buttons);
    uint8_t released = (uint8_t)(changed & cur_buttons);
    cur_buttons = buttons;

    // A report with no motion, no button change and no wheel says nothing.
    if (!dx && !dy && !wheel && !changed) return;

    struct mouse_event *e = &queue[q_head];
    e->x = cur_x;
    e->y = cur_y;
    e->buttons  = cur_buttons;
    e->pressed  = pressed;
    e->released = released;
    e->wheel    = (int8_t)wheel;

    q_head = (q_head + 1) % MOUSE_QUEUE;
    if (q_head == q_tail) q_tail = (q_tail + 1) % MOUSE_QUEUE;   // drop oldest
}

int mouse_poll_event(struct mouse_event *out) {
    if (q_tail == q_head) return 0;
    if (out) *out = queue[q_tail];
    q_tail = (q_tail + 1) % MOUSE_QUEUE;
    return 1;
}
