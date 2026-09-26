// HID report descriptors, as far as a mouse needs them. See hid.h.
//
// A report descriptor is a little program of items, one to five bytes each:
// global items set state that lasts (usage page, logical range, report size,
// count and ID), local items name the usages of the next main item, and a
// main item -- Input, for us -- lays down that many fields of that size, one
// after another, in the report with the current ID. Walking it and keeping a
// bit position per report ID is enough to find where every field of an input
// report is.

#include "hid.h"

#define USAGE(page, id) (((uint32_t)(page) << 16) | (id))
#define U_X      USAGE(0x01, 0x30)
#define U_Y      USAGE(0x01, 0x31)
#define U_WHEEL  USAGE(0x01, 0x38)
#define U_BUTTON 0x00090000u

void hid_mouse_boot(hid_mouse_fmt *f) {
    f->id = 0;
    f->btn[0] = 0; f->btn[1] = 1; f->btn[2] = 2;
    f->x = 8;      f->xs = 8; f->xsig = 1;
    f->y = 16;     f->ys = 8; f->ysig = 1;
    f->wheel = 24; f->ws = 8; f->wsig = 1;
}

int hid_parse_mouse(const uint8_t *d, int len, hid_mouse_fmt *f) {
    static uint16_t pos[256];            // next input bit, per report ID
    for (int i = 0; i < 256; i++) pos[i] = 0;

    uint32_t page = 0, rsize = 0, rcount = 0;
    int32_t lmin = 0;
    uint8_t rid = 0;
    uint32_t usages[16];
    int nus = 0;
    uint32_t umin = 0, umax = 0;
    int range = 0;                       // 1: min seen, 2: max seen

    int x_rid = -1, y_rid = -1, w_rid = -1, b_rid[3] = { -1, -1, -1 };
    f->x = f->y = f->wheel = -1;
    f->btn[0] = f->btn[1] = f->btn[2] = -1;
    f->xs = f->ys = f->ws = 0;
    f->xsig = f->ysig = f->wsig = 0;

    for (int i = 0; i < len; ) {
        uint8_t p = d[i];
        if (p == 0xFE) {                 // a long item: skip it whole
            if (i + 1 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = p & 3;
        if (size == 3) size = 4;
        int type = (p >> 2) & 3, tag = p >> 4;
        if (i + 1 + size > len) break;
        uint32_t u = 0;
        for (int k = 0; k < size; k++) u |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t s = (int32_t)u;
        if (size == 1) s = (int8_t)u;
        else if (size == 2) s = (int16_t)u;
        i += 1 + size;

        if (type == 1) {                                   // global
            if (tag == 0x0) page = u;
            else if (tag == 0x1) lmin = s;
            else if (tag == 0x7) rsize = u;
            else if (tag == 0x9) rcount = u;
            else if (tag == 0x8) rid = (uint8_t)u;
        } else if (type == 2) {                            // local
            uint32_t full = size == 4 ? u : USAGE(page, u);
            if (tag == 0x0) { if (nus < 16) usages[nus++] = full; }
            else if (tag == 0x1) { umin = full; range |= 1; }
            else if (tag == 0x2) { umax = full; range |= 2; }
        } else if (type == 0) {                            // main
            if (tag == 0x8) {                              // Input
                int constant = u & 1, variable = u & 2, relative = u & 4;
                for (uint32_t k = 0; k < rcount && k < 1024; k++) {
                    uint32_t usage = 0;
                    if (!constant) {
                        if (nus) usage = usages[k < (uint32_t)nus ? k : (uint32_t)nus - 1];
                        else if (range == 3 && umin + k <= umax) usage = umin + k;
                    }
                    int bit = pos[rid];
                    if (variable && usage) {
                        if (usage == U_X && relative && x_rid < 0) {
                            f->x = (int16_t)bit; f->xs = (uint8_t)rsize;
                            f->xsig = lmin < 0; x_rid = rid;
                        } else if (usage == U_Y && relative && y_rid < 0) {
                            f->y = (int16_t)bit; f->ys = (uint8_t)rsize;
                            f->ysig = lmin < 0; y_rid = rid;
                        } else if (usage == U_WHEEL && relative && w_rid < 0) {
                            f->wheel = (int16_t)bit; f->ws = (uint8_t)rsize;
                            f->wsig = lmin < 0; w_rid = rid;
                        } else if (usage >= U_BUTTON + 1 && usage <= U_BUTTON + 3) {
                            int b = (int)(usage - U_BUTTON - 1);
                            if (b_rid[b] < 0) { f->btn[b] = (int16_t)bit; b_rid[b] = rid; }
                        }
                    }
                    pos[rid] = (uint16_t)(pos[rid] + rsize);
                }
            }
            if (tag == 0x8 || tag == 0x9 || tag == 0xB || tag == 0xA || tag == 0xC) {
                nus = 0;                                   // locals last one item
                range = 0;
            }
        }
    }

    if (x_rid < 0 || y_rid != x_rid) { f->x = f->y = -1; return 0; }
    // Everything else must be in the same report as X and Y to be read with
    // them; a wheel or button described in some other report is left out.
    if (w_rid != x_rid) f->wheel = -1;
    for (int b = 0; b < 3; b++) if (b_rid[b] != x_rid) f->btn[b] = -1;
    f->id = (uint8_t)x_rid;
    return 1;
}

// `size` bits starting at bit `off` of the first `n` bytes of `r`, least
// significant first as HID lays them out. Bits past the end read as 0.
static int32_t field(const uint8_t *r, int n, int off, int size, int sig) {
    if (off < 0 || size <= 0 || size > 32) return 0;
    uint32_t v = 0;
    for (int b = 0; b < size; b++) {
        int at = off + b;
        if (at / 8 >= n) break;
        if ((r[at / 8] >> (at % 8)) & 1) v |= 1u << b;
    }
    if (sig && size < 32 && ((v >> (size - 1)) & 1)) v |= ~0u << size;
    return (int32_t)v;
}

int hid_mouse_read(const hid_mouse_fmt *f, const uint8_t *r, int len,
                   int *buttons, int *dx, int *dy, int *wheel) {
    if (f->id) {
        if (len < 1 || r[0] != f->id) return 0;
        r++;
        len--;
    }
    int b = 0;
    for (int k = 0; k < 3; k++)
        if (f->btn[k] >= 0 && field(r, len, f->btn[k], 1, 0)) b |= 1 << k;
    *buttons = b;
    *dx = field(r, len, f->x, f->xs, f->xsig);
    *dy = field(r, len, f->y, f->ys, f->ysig);
    *wheel = f->wheel >= 0 ? field(r, len, f->wheel, f->ws, f->wsig) : 0;
    return 1;
}
