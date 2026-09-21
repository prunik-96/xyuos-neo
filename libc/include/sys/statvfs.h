#ifndef SYS_STATVFS_H
#define SYS_STATVFS_H

/* How much room is left on the filesystem.
 *
 * The kernel can answer this -- it knows the block counts -- but has no system
 * call that says so yet, so statvfs reports the block size it does know and
 * zero for the counts. A program that asks gets a truthful "I do not know"
 * rather than a made-up number that would look like a full disk or an empty
 * one depending on which way it was invented. */

struct statvfs {
    unsigned long f_bsize;      /* block size                    */
    unsigned long f_frsize;     /* fundamental block size        */
    unsigned long f_blocks;     /* blocks in total, 0 if unknown */
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files;      /* inodes in total               */
    unsigned long f_ffree;
    unsigned long f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *out);
int fstatvfs(int fd, struct statvfs *out);

#endif
