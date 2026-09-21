#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// Unbuffered streams over the POSIX descriptors from libc/src/posix.c:
// 0/1/2 are stdin/stdout/stderr and real files start at 3.
typedef struct {
    long fd;
    int  eof;
    int  err;
    int  ungot;   // pushed-back character, or -1 when empty
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

// --- streams ---
FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    fflush(FILE *f);          // no-op: streams are unbuffered
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    remove(const char *path);

// --- character / line I/O ---
int    fgetc(FILE *f);
int    getc(FILE *f);
int    ungetc(int c, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
char  *fgets(char *buf, int size, FILE *f);
int    putchar(int c);
int    puts(const char *s);
int    getchar(void);

/* Formatted input. Terminal reads are line-at-a-time and echoed by the
 * kernel, so a blocking scanf() lets the user see and correct what they type. */
int    scanf(const char *fmt, ...);
int    sscanf(const char *str, const char *fmt, ...);
int    fscanf(FILE *f, const char *fmt, ...);
int    vsscanf(const char *str, const char *fmt, __builtin_va_list ap);
int    vfscanf(FILE *f, const char *fmt, __builtin_va_list ap);

// --- formatted output ---
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int vfprintf(FILE *f, const char *fmt, va_list args);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

// Reads a line from the keyboard into buf (max including the NUL
// terminator), with backspace support and echo. Returns the length.
/* Not standard C -- our own line editor. The name is a common one and
 * ported code often has its own, so a program can ask for that name back by
 * defining XYUOS_NO_READLINE before including this header. */
#ifndef XYUOS_NO_READLINE
size_t readline(char *buf, size_t max);
#endif

#ifdef __cplusplus
}
#endif

#endif
