#include "panic.h"
#include "kio.h"
#include "process.h"
#include "../drivers/framebuffer.h"
#include "../drivers/font8x8_basic.h"
#include "../arch/x86_64/pit.h"
#include <stdint.h>

// ---- palette ----
#define BG      0x00071A2Cu   // deep blue
#define PANEL   0x000E2A44u
#define ACCENT  0x0033B5E5u   // cyan
#define WHITE   0x00E8EEF5u
#define DIM     0x008FA3B8u
#define RED     0x00FF5A5Au
#define YELLOW  0x00F0C674u
#define GREEN   0x0090C978u

// ---- tiny formatters ----
static const char *HEX = "0123456789abcdef";

static void u64hex(char *o, uint64_t v) {          // fixed 16-digit
    for (int i = 15; i >= 0; i--) { o[i] = HEX[v & 0xF]; v >>= 4; }
    o[16] = 0;
}
static void udec(char *o, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) { o[0] = '0'; o[1] = 0; return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (n) o[j++] = t[--n]; o[j] = 0;
}

// ---- scaled glyph drawing (font8x8, blown up so it reads on a big panel) ----
static void put_glyph(uint32_t x, uint32_t y, char c, uint32_t s, uint32_t fg) {
    unsigned uc = (unsigned char)c;
    if (uc >= 128) uc = '?';
    const uint8_t *g = font8x8_basic[uc];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << (7 - col)))   // font is MSB=leftmost (match fb console)
                fb_fill_rect(x + col * s, y + row * s, s, s, fg);
    }
}
static uint32_t text(uint32_t x, uint32_t y, const char *str, uint32_t s, uint32_t fg) {
    for (const char *p = str; *p; p++) { put_glyph(x, y, *p, s, fg); x += 8 * s; }
    return x;
}
// Draw "LABEL <hex>" as a labelled 64-bit register. Returns nothing.
static void reg(uint32_t x, uint32_t y, const char *label, uint64_t v, uint32_t s) {
    char h[17]; u64hex(h, v);
    uint32_t nx = text(x, y, label, s, DIM);
    nx = text(nx, y, " 0x", s, DIM);
    text(nx, y, h, s, WHITE);
}

void panic_screen(const char *reason, struct interrupt_frame *frame) {
    if (!fb_available()) return;

    uint64_t cr2, cr3;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    uint32_t W = fb_get_width(), H = fb_get_height();

    // Backdrop + a header band.
    fb_fill_rect(0, 0, W, H, BG);
    fb_fill_rect(0, 0, W, 96, PANEL);
    fb_fill_rect(0, 96, W, 3, ACCENT);

    // Header: sad face + title.
    text(40, 20, ":(", 7, ACCENT);
    text(150, 22, "KERNEL PANIC", 5, WHITE);
    text(152, 66, "xyuOS Neo stopped to protect the system", 2, DIM);

    uint32_t y = 128;
    const uint32_t S = 2;          // body text scale (16 px cells)
    const uint32_t LH = 8 * S + 6; // line height
    const uint32_t X = 40;

    // ---- FAULT ----
    text(X, y, "FAULT", S, ACCENT); y += LH;
    {
        char n[24];
        uint32_t nx = text(X, y, reason ? reason : "Exception", S, RED);
        nx = text(nx + 16, y, "int=", S, DIM);
        udec(n, frame->int_no); nx = text(nx, y, n, S, WHITE);
        nx = text(nx + 16, y, "err=0x", S, DIM);
        u64hex(n, frame->err_code); nx = text(nx, y, n + 13, S, WHITE); // low 3 nibbles
        y += LH;
        if (frame->int_no == 14) {   // page fault: decode the error code
            nx = text(X, y, "cause: ", S, DIM);
            nx = text(nx, y, (frame->err_code & 1) ? "protection " : "not-present ", S, YELLOW);
            nx = text(nx, y, (frame->err_code & 2) ? "write " : "read ", S, YELLOW);
            nx = text(nx, y, (frame->err_code & 4) ? "user" : "kernel", S, YELLOW);
            y += LH;
        }
    }
    y += 8;

    // ---- CONTEXT ----
    text(X, y, "CONTEXT", S, ACCENT); y += LH;
    {
        process_t *p = process_current();
        char n[24];
        uint32_t nx = text(X, y, "process: ", S, DIM);
        if (p) {
            udec(n, (uint64_t)(uint32_t)p->pid);
            nx = text(nx, y, "#", S, WHITE);
            nx = text(nx, y, n, S, WHITE);
            nx = text(nx + 12, y, p->name[0] ? p->name : "(unnamed)", S, WHITE);
        } else {
            nx = text(nx, y, "kernel (no process)", S, WHITE);
        }
        y += LH;
        uint64_t up = pit_get_ticks() / 100;   // 100 Hz -> seconds
        char h[8], m[8], s[8];
        udec(h, up / 3600); udec(m, (up / 60) % 60); udec(s, up % 60);
        nx = text(X, y, "uptime: ", S, DIM);
        nx = text(nx, y, h, S, WHITE); nx = text(nx, y, "h ", S, DIM);
        nx = text(nx, y, m, S, WHITE); nx = text(nx, y, "m ", S, DIM);
        nx = text(nx, y, s, S, WHITE); nx = text(nx, y, "s", S, DIM);
        y += LH + 8;
    }

    // ---- REGISTERS (two columns) ----
    text(X, y, "REGISTERS", S, ACCENT); y += LH;
    {
        uint32_t colL = X, colR = X + 520;
        uint32_t ry = y;
        reg(colL, ry, "RIP", frame->rip, S);   reg(colR, ry, "RFLAGS", frame->rflags, S); ry += LH;
        reg(colL, ry, "RSP", frame->rsp, S);   reg(colR, ry, "CR2   ", cr2, S);           ry += LH;
        reg(colL, ry, "RBP", frame->rbp, S);   reg(colR, ry, "CR3   ", cr3, S);           ry += LH;
        reg(colL, ry, "CS ", frame->cs, S);    reg(colR, ry, "SS    ", frame->ss, S);     ry += LH;
        ry += 6;
        reg(colL, ry, "RAX", frame->rax, S);   reg(colR, ry, "RBX   ", frame->rbx, S);    ry += LH;
        reg(colL, ry, "RCX", frame->rcx, S);   reg(colR, ry, "RDX   ", frame->rdx, S);    ry += LH;
        reg(colL, ry, "RSI", frame->rsi, S);   reg(colR, ry, "RDI   ", frame->rdi, S);    ry += LH;
        reg(colL, ry, "R8 ", frame->r8,  S);   reg(colR, ry, "R9    ", frame->r9,  S);    ry += LH;
        reg(colL, ry, "R10", frame->r10, S);   reg(colR, ry, "R11   ", frame->r11, S);    ry += LH;
        reg(colL, ry, "R12", frame->r12, S);   reg(colR, ry, "R13   ", frame->r13, S);    ry += LH;
        reg(colL, ry, "R14", frame->r14, S);   reg(colR, ry, "R15   ", frame->r15, S);    ry += LH;
        y = ry + 10;
    }

    // ---- RECENT KERNEL LOG ----
    text(X, y, "RECENT KERNEL LOG", S, ACCENT); y += LH;
    {
        const uint32_t LOGLINES = 14, LOGCOLS = 120;
        static char line[14][121];
        for (uint32_t i = 0; i < LOGLINES; i++) line[i][0] = 0;

        const char *lb; uint32_t sz, head;
        klog_snapshot(&lb, &sz, &head);
        uint32_t avail = head < sz ? head : sz;
        uint32_t start = head - avail;
        uint32_t cur = 0, col = 0;
        for (uint32_t i = 0; i < avail; i++) {
            char c = lb[(start + i) % sz];
            if (c == '\n') { line[cur][col] = 0; cur = (cur + 1) % LOGLINES; col = 0; line[cur][0] = 0; }
            else if (c == '\r') { /* skip */ }
            else if (col < LOGCOLS) { line[cur][col++] = c; line[cur][col] = 0; }
        }
        // Panel behind the log.
        uint32_t panel_y = y - 4;
        fb_fill_rect(X - 12, panel_y, W - 2 * (X - 12), LOGLINES * (8 + 4) + 12, PANEL);
        // Render oldest..newest (the ring's next slot after `cur` is the oldest).
        for (uint32_t i = 0; i < LOGLINES; i++) {
            uint32_t idx = (cur + 1 + i) % LOGLINES;
            if (line[idx][0]) text(X, y, line[idx], 1, DIM);   // scale 1 = dense
            y += 8 + 4;
        }
    }

    // Footer.
    text(X, H - 40, "System halted. Power-cycle the machine to reboot.", S, ACCENT);

    fb_present();   // make sure it reaches the screen even with double buffering

    // We are about to halt, so nothing else will ever flush the framebuffer.
    // Force every pending write out to video memory: sfence drains the
    // write-combining buffers, wbinvd writes back any cached pixels.
    __asm__ volatile ("sfence" ::: "memory");
    __asm__ volatile ("wbinvd" ::: "memory");
}
