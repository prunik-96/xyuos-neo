/* mmap and munmap, and scandir. See sys/mman.h for what mapping means here.
 *
 * These are together because they arrived together: they are what a program
 * needs to read a directory and serve the files in it, which is the file:
 * half of a browser.
 */

#include <sys/mman.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           long offset) {
    /* Placing a mapping at a chosen address needs control over the address
     * space that this does not have. */
    if (addr != NULL || (flags & MAP_FIXED)) { errno = ENOTSUP; return MAP_FAILED; }
    if (offset != 0) { errno = ENOTSUP; return MAP_FAILED; }
    if (length == 0) { errno = EINVAL; return MAP_FAILED; }

    if (flags & MAP_ANONYMOUS) {
        void *p = calloc(1, length);          /* a fresh mapping reads as zero */
        if (p == NULL) { errno = ENOMEM; return MAP_FAILED; }
        return p;
    }

    /* Writes to a shared mapping are supposed to reach the file and every
     * other program that has it mapped. Nothing here can arrange that, and a
     * private copy would look identical right up to the moment it mattered. */
    if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }

    if (fd < 0) { errno = EBADF; return MAP_FAILED; }

    unsigned char *p = malloc(length);
    if (p == NULL) { errno = ENOMEM; return MAP_FAILED; }

    /* Read it now, all of it. Short reads are not an error at the end of a
     * file: a mapping past the end reads as zero, so that is what is left. */
    size_t got = 0;
    while (got < length) {
        long n = read(fd, p + got, length - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (got < length) memset(p + got, 0, length - got);

    return p;
}

int munmap(void *addr, size_t length) {
    (void)length;                  /* the block is freed whole, as it was made */
    if (addr == NULL || addr == MAP_FAILED) { errno = EINVAL; return -1; }
    free(addr);
    return 0;
}

/* --- reading a whole directory ------------------------------------------- */

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

/* Collects every entry the filter accepts into an array the caller frees --
 * each entry, then the array. The comparison function is the one qsort takes,
 * so alphasort above fits it directly. */
int scandir(const char *path, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    if (path == NULL || namelist == NULL) { errno = EINVAL; return -1; }

    DIR *d = opendir(path);
    if (d == NULL) return -1;

    struct dirent **list = NULL;
    size_t n = 0, cap = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (filter != NULL && !filter(e)) continue;

        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            struct dirent **nl = realloc(list, nc * sizeof *nl);
            if (nl == NULL) goto out_of_memory;
            list = nl;
            cap = nc;
        }
        struct dirent *copy = malloc(sizeof *copy);
        if (copy == NULL) goto out_of_memory;
        *copy = *e;                 /* readdir reuses its own entry */
        list[n++] = copy;
    }
    closedir(d);

    if (compar != NULL && n > 1) {
        qsort(list, n, sizeof *list,
              (int (*)(const void *, const void *))compar);
    }

    *namelist = list;
    return (int)n;

out_of_memory:
    for (size_t i = 0; i < n; i++) free(list[i]);
    free(list);
    closedir(d);
    errno = ENOMEM;
    return -1;
}
