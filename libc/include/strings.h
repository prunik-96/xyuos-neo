#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>

// BSD-ish case-insensitive comparisons. Implemented in libc/src/string_extra.c.
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
