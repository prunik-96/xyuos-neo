#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <sys/types.h>

/* File metadata, in the shape POSIX describes it.
 *
 * The filesystem underneath knows three things about a file: its size, its
 * inode number, and whether it is a directory. Those fields carry real
 * information. The rest exist because ported code reads them, and are filled
 * with what is actually true here rather than with invented values --
 * timestamps are zero because nothing records them yet, and the permission
 * bits say what the system really enforces, which is that anyone may read and
 * write anything. */

/* All seven file types, at the values every Unix has used since System V.
 * This system creates only two of them, and the tests below will duly answer
 * no for the rest -- because nothing sets those bits, not because the answer
 * is wired to no. The difference matters: code that switches on the type of
 * a file needs the names to exist in order to compile at all, and a test
 * that cannot come out true is not the same as a test on a bit that is
 * never set. */
#define S_IFMT   0170000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFBLK  0060000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRWXO 0007

struct stat {
    mode_t        st_mode;
    unsigned long st_ino;
    unsigned long st_size;
    unsigned long st_nlink;
    unsigned long st_dev;
    unsigned int  st_uid;
    unsigned int  st_gid;
    long          st_atime;
    long          st_mtime;
    long          st_ctime;
    unsigned long st_blksize;
    unsigned long st_blocks;
};

int stat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);
int lstat(const char *path, struct stat *out);   /* no symlinks: the same */

/* Create a directory. `mode` is accepted and ignored: this system has no
 * permissions to apply it to. */
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);

#endif
