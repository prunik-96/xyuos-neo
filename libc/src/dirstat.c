/* Directories and file metadata, in POSIX clothing.
 *
 * The system calls underneath are simpler than the interface on top of them:
 * one returns a whole directory listing as newline-separated names, another
 * returns three facts about a file. This file is the adapter, and it exists
 * because ported programs expect opendir/readdir/stat and there is no reason
 * they should have to care that this system spells them differently.
 */

#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* --- metadata -------------------------------------------------------------- */

static void fill_stat(const struct xyuos_stat *in, struct stat *out) {
    for (unsigned i = 0; i < sizeof *out; i++) ((char *)out)[i] = 0;
    out->st_mode = (in->is_dir ? S_IFDIR : S_IFREG) | 0666;
    if (in->is_dir) out->st_mode |= 0111;
    out->st_ino = in->inode;
    out->st_size = in->size;
    out->st_nlink = 1;
    out->st_blksize = 1024;
    out->st_blocks = (in->size + 511) / 512;
    /* The filesystem records when a file was last written, so that one is
     * real; the other two are the same value because nothing distinguishes
     * them yet. */
    out->st_mtime = (long)in->mtime;
    out->st_atime = (long)in->mtime;
    out->st_ctime = (long)in->mtime;
}

int stat(const char *path, struct stat *out) {
    struct xyuos_stat st;
    if (xyuos_stat(path, &st) != 0) { errno = ENOENT; return -1; }
    fill_stat(&st, out);
    return 0;
}

int lstat(const char *path, struct stat *out) { return stat(path, out); }

int fstat(int fd, struct stat *out) {
    /* There is no stat-by-descriptor call; the size is what a seek to the end
     * reports, which is the part anybody actually asks for. */
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }
    long end = lseek(fd, 0, SEEK_END);
    lseek(fd, cur, SEEK_SET);
    if (end < 0) { errno = EBADF; return -1; }

    for (unsigned i = 0; i < sizeof *out; i++) ((char *)out)[i] = 0;
    out->st_mode = S_IFREG | 0666;
    out->st_size = (unsigned long)end;
    out->st_nlink = 1;
    out->st_blksize = 1024;
    out->st_blocks = ((unsigned long)end + 511) / 512;
    return 0;
}

/* There is no chdir in this system: each program is given its working
 * directory when it starts and the shell owns the notion of "where you are".
 * Reporting failure is the honest answer; pretending to succeed would leave
 * every relative path afterwards quietly wrong. */
int chdir(const char *path) {
    (void)path;
    errno = ENOSYS;
    return -1;
}

int rmdir(const char *path) {
    /* A directory is removed the same way a file is; the filesystem refuses if
     * it still has anything in it. */
    if (xyuos_unlink(path) != 0) { errno = ENOTEMPTY; return -1; }
    return 0;
}

/* --- directories ----------------------------------------------------------- */

struct _DIR {
    char          *listing;     /* the whole thing, newline separated */
    long           len;
    long           pos;
    struct dirent  ent;
};

DIR *opendir(const char *path) {
    /* Big enough for a directory with a few thousand entries in it; the call
     * reports how much it wrote, so a larger one is simply truncated rather
     * than lost. */
    long cap = 64 * 1024;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) { errno = ENOMEM; return 0; }

    long n = xyuos_listdir(path, buf, (unsigned long)cap);
    if (n <= 0) {
        free(buf);
        errno = ENOENT;
        return 0;
    }

    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { free(buf); errno = ENOMEM; return 0; }
    d->listing = buf;
    d->len = n;
    d->pos = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || d->pos >= d->len) return 0;

    long start = d->pos;
    while (d->pos < d->len && d->listing[d->pos] != '\n') d->pos++;
    long end = d->pos;
    if (d->pos < d->len) d->pos++;          /* step over the newline */

    long n = end - start;
    /* A trailing slash is how the listing marks a directory. */
    int is_dir = (n > 0 && d->listing[end - 1] == '/');
    if (is_dir) n--;
    if (n > NAME_MAX_DIRENT) n = NAME_MAX_DIRENT;

    for (long i = 0; i < n; i++) d->ent.d_name[i] = d->listing[start + i];
    d->ent.d_name[n] = 0;
    d->ent.d_type = is_dir ? DT_DIR : DT_REG;
    d->ent.d_ino = 0;

    /* Skip the two entries every directory has and nobody wants. */
    if (strcmp(d->ent.d_name, ".") == 0 || strcmp(d->ent.d_name, "..") == 0)
        return readdir(d);

    return &d->ent;
}

void rewinddir(DIR *d) { if (d) d->pos = 0; }

int closedir(DIR *d) {
    if (!d) return -1;
    free(d->listing);
    free(d);
    return 0;
}


/* --- waiting, and how much room is left ------------------------------------ */

#include <poll.h>
#include <sys/statvfs.h>

/* Every descriptor here refers to a file on a local disk or to the terminal,
 * and a read from either returns without waiting. So everything asked about is
 * ready, which is the same answer a real poll() gives for ordinary files. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    (void)timeout;
    int n = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
        if (fds[i].revents) n++;
    }
    return n;
}

/* The kernel knows the block counts and has no call that reports them, so the
 * honest answer is the block size and zeros -- not an invented total that
 * would read as a full disk or an empty one depending on the guess. */
int statvfs(const char *path, struct statvfs *out) {
    (void)path;
    for (unsigned i = 0; i < sizeof *out; i++) ((char *)out)[i] = 0;
    out->f_bsize = 1024;
    out->f_frsize = 1024;
    out->f_namemax = 255;
    return 0;
}

int fstatvfs(int fd, struct statvfs *out) {
    (void)fd;
    return statvfs("/", out);
}

/* Writes go straight to the disk driver -- there is no cache between a write
 * and the platter -- so a file is already on disk by the time write() has
 * returned, and there is nothing left for this to flush. */
int fsync(int fd) { (void)fd; return 0; }
int fdatasync(int fd) { (void)fd; return 0; }
