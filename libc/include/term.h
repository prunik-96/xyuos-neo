#ifndef TERM_H
#define TERM_H

#include <stdio.h>

// ANSI color indices (add to 30 for fg, 40 for bg via term_fg/term_bg).
#define TC_BLACK   0
#define TC_RED     1
#define TC_GREEN   2
#define TC_YELLOW  3
#define TC_BLUE    4
#define TC_MAGENTA 5
#define TC_CYAN    6
#define TC_WHITE   7

static inline void term_clear(void)            { printf("\x1b[2J\x1b[1;1H"); }
static inline void term_home(void)             { printf("\x1b[1;1H"); }
static inline void term_move(int row, int col) { printf("\x1b[%d;%dH", row, col); } // 1-based
static inline void term_erase_line(void)       { printf("\x1b[K"); }
static inline void term_reverse(void)          { printf("\x1b[7m"); }
static inline void term_reset(void)            { printf("\x1b[0m"); }
static inline void term_fg(int c)              { printf("\x1b[%dm", 30 + c); }
static inline void term_bg(int c)              { printf("\x1b[%dm", 40 + c); }
static inline void term_fg_bright(int c)       { printf("\x1b[%dm", 90 + c); }

#endif
