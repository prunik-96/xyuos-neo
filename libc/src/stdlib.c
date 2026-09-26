#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* malloc, free, calloc and realloc are in malloc.c. */

/* Defined in posixbits.c, next to atexit itself. */
void __libc_run_atexit(void);

void exit(int code) { __libc_run_atexit(); _exit(code); }
void abort(void)    { _exit(134); }

int atoi(const char *s) {
    int sign = 1;
    int result = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}
