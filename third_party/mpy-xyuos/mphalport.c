#include <stdio.h>
#include <unistd.h>

#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/mphal.h"

/* Characters in, characters out, and a clock -- the whole of what MicroPython
 * asks an operating system for. */

mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)uptime_ms(); }
mp_uint_t mp_hal_ticks_us(void) { return (mp_uint_t)uptime_ms() * 1000u; }

void mp_hal_delay_ms(mp_uint_t ms) { sleep_ms((unsigned)ms); }
void mp_hal_delay_us(mp_uint_t us) { sleep_ms((unsigned)((us + 999) / 1000)); }

int mp_hal_stdin_rx_chr(void) {
    int c = getchar();
    /* The terminal sends carriage return for the Enter key; the reader wants
     * to see it as one, so it is passed through untouched. */
    return c;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    long n = write(1, str, (unsigned long)len);
    return n > 0 ? (mp_uint_t)n : 0;
}

/* Ctrl-C does not arrive asynchronously here -- the terminal hands it over as
 * an ordinary character -- so there is nothing to arm. */
void mp_hal_set_interrupt_char(int c) { (void)c; }
