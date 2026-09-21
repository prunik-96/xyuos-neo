#ifndef DIRENT_H
#define DIRENT_H

/* Walking a directory, one entry at a time.
 *
 * The system call underneath hands back the whole listing in one go -- names
 * separated by newlines, with a trailing slash on the directories. So opendir
 * reads it once and readdir walks what it read. That means a directory does
 * not change under a program that is part way through listing it, which is a
 * difference from a real POSIX system, and a difference in the harmless
 * direction. */

#define NAME_MAX_DIRENT 255

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    unsigned long  d_ino;
    unsigned char  d_type;
    char           d_name[NAME_MAX_DIRENT + 1];
};

typedef struct _DIR DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);

/* The whole listing at once, sorted. The caller frees each entry and then the
 * array. `filter` may be NULL to take everything; `compar` may be NULL to
 * leave the order alone, and alphasort is the usual one to pass. */
int  scandir(const char *path, struct dirent ***namelist,
             int (*filter)(const struct dirent *),
             int (*compar)(const struct dirent **, const struct dirent **));
int  alphasort(const struct dirent **a, const struct dirent **b);

#endif
