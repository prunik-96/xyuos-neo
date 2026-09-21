#ifndef STRING_H
#define STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strpbrk(const char *s, const char *accept);
char *strerror(int e);
void *memchr(const void *s, int c, size_t n);
char  *strdup(const char *s);

/* Splits a string in place, remembering where it got to between calls. The
 * saved position is one global, which is what the interface says it is; the
 * _r form takes its own and is the one to reach for. */
char  *strtok(char *s, const char *sep);
char  *strtok_r(char *s, const char *sep, char **save);
char *strndup(const char *s, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

#ifdef __cplusplus
}
#endif

#endif
