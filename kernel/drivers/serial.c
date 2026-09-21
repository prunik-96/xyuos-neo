#include "serial.h"
#include "../include/port_io.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);    // disable interrupts
    outb(COM1 + 3, 0x80);    // enable DLAB
    outb(COM1 + 0, 0x03);    // divisor low byte -> 38400 baud
    outb(COM1 + 1, 0x00);    // divisor high byte
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}
