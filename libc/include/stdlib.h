#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int code) __attribute__((noreturn));

/* Run on the way out, most recently registered first. Library code registers
 * a tidy-up here and would otherwise not link at all. */
int  atexit(void (*fn)(void));
void abort(void) __attribute__((noreturn));

int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);
long double strtold(const char *s, char **end);
double atof(const char *s);
char  *realpath(const char *path, char *resolved);

int  abs(int v);
long labs(long v);

/* Its opposite number, and the one thing NetSurf's CSS engine wanted that
 * this libc had not got: it looks every property name up this way. */
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*cmp)(const void *, const void *));

/* The largest rand() will return. */
#define RAND_MAX 0x7FFFFFFF

int  rand(void);
void srand(unsigned seed);

void qsort(void *base, size_t count, size_t size,
           int (*cmp)(const void *, const void *));

// Always returns NULL: this OS has no environment.
char *getenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif
