#include "vfs.h"
#include "ext2.h"
#include "vol.h"
#include "fatfs.h"
#include "../drivers/blkdev.h"
#include "../mm/heap.h"
#include <stddef.h>

#define MAX_OPEN_FILES 16

// The root filesystem is ext2 (on bare metal the partition on the boot stick).
// The filesystems on other USB drives are grafted in at /usb, /usb2, ...
// (see vol.h): FAT16, FAT32 and exFAT, written as they are written to, so a
// stick pulled out holds everything closed before it went.
#define FS_EXT2 0
#define FS_VOL  1

typedef struct {
    int used;
    int fs;
    uint32_t pos;

    // ext2
    uint32_t inode_num;
    ext2_inode_t inode;

    // a volume: which one (and which drive it was: a stick pulled out and
    // another put in its place is not the same file), and the file on it
    int vol;
    uint32_t vgen;
    int dead;                       // deleted, or its drive went: nothing more
    struct ffile ff;
} open_file_t;

static open_file_t open_files[MAX_OPEN_FILES];

int vfs_init(uint32_t multiboot_addr) {
    if (!blkdev_init(multiboot_addr)) return 0;
    return ext2_mount();
}

// --- volumes -----------------------------------------------------------------

static int alloc_slot(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (!open_files[i].used) return i;
    return -1;
}

// The volume an open file is on, if it is still the one it was opened on.
static struct vol *file_vol(open_file_t *f) {
    if (f->dead || vol_gen(f->vol) != f->vgen) return NULL;
    struct vol *v = vol_get(f->vol);
    if (!v || vol_gen(f->vol) != f->vgen) { f->dead = 1; return NULL; }
    return v;
}

// The same file as `ff` on volume `vi`: the same entry in the same directory.
static int same_file(const open_file_t *f, int vi, const struct ffile *ff) {
    return f->used && f->fs == FS_VOL && !f->dead && f->vol == vi &&
           f->ff.ent == ff->ent && f->ff.in.first == ff->in.first &&
           f->ff.in.fixed == ff->in.fixed;
}

static int vol_open_path(const char *path, int *vi, struct vol **v, const char **rest) {
    *vi = vol_of_path(path, rest);
    if (*vi < 0) return 0;
    *v = vol_get(*vi);
    return *v != NULL;
}

int vfs_open(const char *path) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        if (!vol_open_path(path, &vi, &v, &rest)) return -1;
        int fd = alloc_slot();
        if (fd < 0) return -1;
        open_file_t *f = &open_files[fd];
        if (!fatfs_open(v, rest, &f->ff)) return -1;
        f->used = 1;
        f->fs = FS_VOL;
        f->pos = 0;
        f->vol = vi;
        f->vgen = vol_gen(vi);
        f->dead = 0;
        return fd;
    }

    uint32_t inode_num;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &inode_num, &inode)) return -1;

    int fd = alloc_slot();
    if (fd < 0) return -1;
    open_file_t *f = &open_files[fd];
    f->used = 1;
    f->fs = FS_EXT2;
    f->inode_num = inode_num;
    f->inode = inode;
    f->pos = 0;
    return fd;
}

int32_t vfs_read(int fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_VOL) {
        struct vol *v = file_vol(f);
        if (!v) return -1;
        long n = fatfs_pread(v, &f->ff, f->pos, buf, len);
        if (n < 0) return -1;
        f->pos += (uint32_t)n;
        return (int32_t)n;
    }

    uint32_t n = ext2_read(&f->inode, f->pos, buf, len);
    f->pos += n;
    return (int32_t)n;
}

int32_t vfs_write(int fd, const void *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_VOL) {
        if (len == 0) return 0;
        struct vol *v = file_vol(f);
        if (!v) return -1;
        long n = fatfs_pwrite(v, &f->ff, f->pos, buf, len);
        if (n < 0) return -1;
        f->pos += (uint32_t)n;
        return (int32_t)n;
    }

    int n = ext2_write(f->inode_num, f->pos, buf, len);
    if (n < 0) return -1;
    f->pos += (uint32_t)n;
    // Refresh the cached inode so size/blocks stay accurate for later reads.
    ext2_get_inode(f->inode_num, &f->inode);
    return n;
}

void vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return;
    open_file_t *f = &open_files[fd];
    if (f->used && f->fs == FS_VOL) {
        struct vol *v = file_vol(f);
        if (v) fatfs_sync(v, &f->ff);          // its size and time, into its entry
    }
    f->used = 0;
}

// --- random access ---------------------------------------------------------

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    int64_t base;
    switch (whence) {
        case VFS_SEEK_SET: base = 0; break;
        case VFS_SEEK_CUR: base = (int64_t)f->pos; break;
        case VFS_SEEK_END: base = (int64_t)(f->fs == FS_VOL ? f->ff.size : f->inode.i_size); break;
        default: return -1;
    }

    int64_t target = base + offset;
    if (target < 0) return -1;              // seeking before the start is an error
    // Files are limited to 32-bit sizes here (ext2 rev0 layout as we use it).
    if (target > (int64_t)0xFFFFFFFF) return -1;

    f->pos = (uint32_t)target;
    return target;
}

int64_t vfs_tell(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    return (int64_t)open_files[fd].pos;
}

// --- metadata --------------------------------------------------------------

static void fill_stat(struct vfs_stat *out, uint32_t ino, const ext2_inode_t *in) {
    out->size   = in->i_size;
    out->inode  = ino;
    out->is_dir = ((in->i_mode & 0xF000) == EXT2_S_IFDIR) ? 1 : 0;
    out->mtime  = in->i_mtime;
}

int vfs_stat(const char *path, struct vfs_stat *out) {
    if (!out) return -1;

    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        static struct ffile ff;
        if (!vol_open_path(path, &vi, &v, &rest) || !fatfs_open(v, rest, &ff)) return -1;
        out->size   = (uint32_t)(ff.size > 0xFFFFFFFFull ? 0xFFFFFFFFull : ff.size);
        out->inode  = ff.first;     // no inodes on FAT; the first cluster is the id
        out->is_dir = ff.is_dir;
        out->mtime  = (uint32_t)fatfs_mtime(v, &ff);
        return 0;
    }

    uint32_t ino;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &ino, &inode)) return -1;
    fill_stat(out, ino, &inode);
    return 0;
}

int vfs_truncate(const char *path, uint32_t length) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        static struct ffile ff;
        if (!vol_open_path(path, &vi, &v, &rest) || !fatfs_open(v, rest, &ff)) return -1;
        // An fd open on it is the file's truth: truncate through that.
        for (int i = 0; i < MAX_OPEN_FILES; i++)
            if (same_file(&open_files[i], vi, &ff)) return vfs_ftruncate(i, length);
        if (!fatfs_truncate(v, &ff, length) || !fatfs_sync(v, &ff)) return -1;
        return 0;
    }

    uint32_t ino;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &ino, &inode)) return -1;
    if (!ext2_truncate(ino, length)) return -1;

    // Any fd already open on this inode must see the new size, and its cursor
    // must not be left dangling past the end.
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].used && open_files[i].fs == FS_EXT2 && open_files[i].inode_num == ino) {
            ext2_get_inode(ino, &open_files[i].inode);
            if (open_files[i].pos > length) open_files[i].pos = length;
        }
    }
    return 0;
}

int vfs_ftruncate(int fd, uint32_t length) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_VOL) {
        struct vol *v = file_vol(f);
        if (!v || !fatfs_truncate(v, &f->ff, length) || !fatfs_sync(v, &f->ff)) return -1;
        if (f->pos > length) f->pos = length;
        return 0;
    }

    if (!ext2_truncate(f->inode_num, length)) return -1;
    ext2_get_inode(f->inode_num, &f->inode);
    if (f->pos > length) f->pos = length;
    return 0;
}

int vfs_fstat(int fd, struct vfs_stat *out) {
    if (!out || fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_VOL) {
        struct vol *v = file_vol(f);
        if (!v) return -1;
        out->size   = (uint32_t)(f->ff.size > 0xFFFFFFFFull ? 0xFFFFFFFFull : f->ff.size);
        out->inode  = f->ff.first;
        out->is_dir = f->ff.is_dir;
        out->mtime  = (uint32_t)fatfs_mtime(v, &f->ff);
        return 0;
    }

    // Re-read the inode: a write through another fd may have changed the size.
    ext2_get_inode(f->inode_num, &f->inode);
    fill_stat(out, f->inode_num, &f->inode);
    return 0;
}

// --- path helpers ---

static uint32_t vstrlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

// Split an absolute path into its parent directory path and leaf name.
// "/a/b/c" -> parent "/a/b", leaf "c";  "/foo" -> parent "/", leaf "foo".
// Returns 0 if the path has no leaf (e.g. "/" or "").
static int split_path(const char *path, char *parent, uint32_t parent_cap, char *leaf, uint32_t leaf_cap) {
    uint32_t len = vstrlen(path);
    if (len == 0) return 0;
    // strip a single trailing slash (but keep root)
    if (len > 1 && path[len - 1] == '/') len--;

    int slash = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }
    if (slash < 0) {
        // relative leaf only; parent is "."
        if (len + 1 > leaf_cap) return 0;
        for (uint32_t i = 0; i < len; i++) leaf[i] = path[i];
        leaf[len] = '\0';
        parent[0] = '.'; parent[1] = '\0';
        return 1;
    }

    uint32_t leaf_len = len - (uint32_t)slash - 1;
    if (leaf_len == 0 || leaf_len + 1 > leaf_cap) return 0;
    for (uint32_t i = 0; i < leaf_len; i++) leaf[i] = path[slash + 1 + i];
    leaf[leaf_len] = '\0';

    uint32_t plen = (slash == 0) ? 1 : (uint32_t)slash; // keep "/" for root
    if (plen + 1 > parent_cap) return 0;
    if (slash == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        for (uint32_t i = 0; i < plen; i++) parent[i] = path[i];
        parent[plen] = '\0';
    }
    return 1;
}

static int lookup_dir_inode(const char *dir_path, uint32_t *out_ino) {
    ext2_inode_t inode;
    return ext2_lookup(dir_path, out_ino, &inode);
}

int vfs_create(const char *path) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        static struct ffile ff;
        if (!vol_open_path(path, &vi, &v, &rest)) return -1;
        if (fatfs_open(v, rest, &ff)) return ff.is_dir ? -1 : 0;   // there already
        return fatfs_create(v, rest, 0, &ff) ? 0 : -1;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_create(pino, leaf, 0, NULL) ? 0 : -1;
}

int vfs_mkdir(const char *path) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        static struct ffile ff;
        if (!vol_open_path(path, &vi, &v, &rest)) return -1;
        return fatfs_create(v, rest, 1, &ff) ? 0 : -1;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_create(pino, leaf, 1, NULL) ? 0 : -1;
}

int vfs_unlink(const char *path) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        static struct ffile ff;
        if (!vol_open_path(path, &vi, &v, &rest) || !fatfs_open(v, rest, &ff)) return -1;
        if (!fatfs_unlink(v, rest)) return -1;
        // An fd still open on it must not write into clusters given away.
        for (int i = 0; i < MAX_OPEN_FILES; i++)
            if (same_file(&open_files[i], vi, &ff)) open_files[i].dead = 1;
        return 0;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_unlink(pino, leaf) ? 0 : -1;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    const char *orest, *nrest;
    int ovi = vol_of_path(oldpath, &orest), nvi = vol_of_path(newpath, &nrest);
    // Renaming ACROSS filesystems would mean copying data between them,
    // which rename() does not do (POSIX calls this EXDEV); use cp.
    if ((ovi >= 0 || nvi >= 0) && ovi != nvi) return -1;
    if (ovi >= 0) {
        static struct ffile ff;
        struct vol *v = vol_get(ovi);
        if (!v || !fatfs_open(v, orest, &ff)) return -1;
        // Files open on it: their entries are written now, and found again
        // under the new name after.
        int moving[MAX_OPEN_FILES], nm = 0;
        for (int i = 0; i < MAX_OPEN_FILES; i++)
            if (same_file(&open_files[i], ovi, &ff)) {
                fatfs_sync(v, &open_files[i].ff);
                moving[nm++] = i;
            }
        if (!fatfs_rename(v, orest, nrest)) return -1;
        if (nm && fatfs_open(v, nrest, &ff)) {
            for (int k = 0; k < nm; k++) {
                struct ffile *o = &open_files[moving[k]].ff;
                o->in = ff.in;
                o->ent = ff.ent;
                o->nents = ff.nents;
            }
        }
        return 0;
    }

    char op[256], ol[256], np[256], nl[256];
    if (!split_path(oldpath, op, sizeof(op), ol, sizeof(ol))) return -1;
    if (!split_path(newpath, np, sizeof(np), nl, sizeof(nl))) return -1;
    uint32_t opi, npi;
    if (!lookup_dir_inode(op, &opi)) return -1;
    if (!lookup_dir_inode(np, &npi)) return -1;
    return ext2_move(opi, ol, npi, nl) ? 0 : -1;
}

typedef struct {
    char *buf;
    uint32_t cap;
    uint32_t written;
} list_ctx_t;

static void list_cb(const char *name, uint32_t name_len, int is_dir, void *userdata) {
    list_ctx_t *ctx = (list_ctx_t *)userdata;
    uint32_t need = name_len + (is_dir ? 1 : 0) + 1;
    if (ctx->written + need > ctx->cap) return;
    for (uint32_t i = 0; i < name_len; i++) {
        ctx->buf[ctx->written++] = name[i];
    }
    if (is_dir) {
        ctx->buf[ctx->written++] = '/';
    }
    ctx->buf[ctx->written++] = '\n';
}

static void ext2_list_cb(const char *name, uint8_t name_len, int is_dir, void *userdata) {
    list_cb(name, name_len, is_dir, userdata);
}

static void vol_list_cb(const char *name, int is_dir, uint64_t size, int64_t mtime, void *userdata) {
    (void)size; (void)mtime;
    list_cb(name, vstrlen(name), is_dir, userdata);
}

uint32_t vfs_list_dir(const char *path, char *out_buf, uint32_t out_buf_len) {
    const char *rest;
    int vi;
    struct vol *v;
    if (vol_of_path(path, &rest) >= 0) {
        if (!vol_open_path(path, &vi, &v, &rest)) return 0;
        list_ctx_t ctx = { out_buf, out_buf_len, 0 };
        fatfs_list(v, rest, vol_list_cb, &ctx);
        return ctx.written;
    }

    uint32_t inode_num;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &inode_num, &inode)) return 0;

    list_ctx_t ctx = { out_buf, out_buf_len, 0 };
    ext2_iterate_dir(&inode, ext2_list_cb, &ctx);

    // The mount points, when listing the root: the drives are discoverable
    // without knowing they are there.
    if (path[0] == '/' && path[1] == '\0') {
        char names[VOL_MAX][8];
        int n = vol_mounts(names, VOL_MAX);
        for (int i = 0; i < n; i++) list_cb(names[i], vstrlen(names[i]), 1, &ctx);
    }

    return ctx.written;
}
