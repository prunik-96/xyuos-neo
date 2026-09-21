#include "vfs.h"
#include "ext2.h"
#include "fat32.h"
#include "../drivers/blkdev.h"
#include "../drivers/xhci.h"
#include "../mm/heap.h"
#include <stddef.h>

#define MAX_OPEN_FILES 16

// The root filesystem is ext2 (a RAM disk on bare metal, so it does NOT survive
// a reboot). The USB stick's FAT32 is grafted into the namespace at /usb, which
// is what gives the system persistent storage: anything written under /usb is
// on the physical stick and is still there after a power cycle.
#define FS_EXT2 0
#define FS_FAT  1

#define USB_MOUNT     "/usb"
#define FAT_PATH_MAX  128
#define FAT_WBUF_MAX  (16u * 1024 * 1024)

typedef struct {
    int used;
    int fs;
    uint32_t pos;

    // ext2
    uint32_t inode_num;
    ext2_inode_t inode;

    // fat32: the path relative to the FAT root, plus a lazily created write
    // buffer. Reads stream straight off the stick (fat32_read takes an offset),
    // so only writing needs to hold the file in memory; it is flushed back as a
    // whole file on close, because the FAT driver rewrites files wholesale.
    char     path[FAT_PATH_MAX];
    uint32_t size;
    uint8_t *wbuf;
    uint32_t wcap;
    int      dirty;
} open_file_t;

static open_file_t open_files[MAX_OPEN_FILES];

int vfs_init(uint32_t multiboot_addr) {
    if (!blkdev_init(multiboot_addr)) return 0;
    return ext2_mount();
}

// --- the /usb mount point ---------------------------------------------------

static void vstrcpy(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int vfs_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// If `path` lies under /usb, return it rebased onto the FAT root ("/usb" -> "/",
// "/usb/a/b" -> "/a/b"). Returns NULL for anything else, including "/usbfoo".
static const char *usb_subpath(const char *path) {
    const char *p = path, *m = USB_MOUNT;
    while (*m && *p == *m) { p++; m++; }
    if (*m) return NULL;
    if (*p == '\0') return "/";
    if (*p != '/') return NULL;
    return p;
}

// Mount the stick on first use. Deliberately lazy: a normal boot never issues
// SCSI reads, exactly as before this mount point existed.
static int usb_ready(void) {
    return fat32_mounted() ? 1 : fat32_automount();
}

static int alloc_slot(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (!open_files[i].used) return i;
    return -1;
}

// Make the write buffer exist and hold at least `need` bytes. The first call
// pulls in the file's current contents, so writing part of a file does not
// discard the rest of it.
static int fat_buf_ensure(open_file_t *f, uint32_t need) {
    if (need > FAT_WBUF_MAX) return 0;

    if (!f->wbuf) {
        uint32_t cap = f->size > need ? f->size : need;
        if (cap < 4096) cap = 4096;
        uint8_t *b = (uint8_t *)kmalloc(cap);
        if (!b) return 0;
        for (uint32_t i = 0; i < cap; i++) b[i] = 0;
        if (f->size) {
            long got = fat32_read(f->path, 0, b, f->size);
            if (got < 0) { kfree(b); return 0; }
        }
        f->wbuf = b;
        f->wcap = cap;
        return 1;
    }

    if (need <= f->wcap) return 1;
    uint32_t cap = f->wcap;
    while (cap < need && cap < FAT_WBUF_MAX) cap *= 2;
    if (cap < need) return 0;
    uint8_t *b = (uint8_t *)kmalloc(cap);
    if (!b) return 0;
    for (uint32_t i = 0; i < f->size && i < cap; i++) b[i] = f->wbuf[i];
    for (uint32_t i = f->size; i < cap; i++) b[i] = 0;
    kfree(f->wbuf);
    f->wbuf = b;
    f->wcap = cap;
    return 1;
}

static void fat_flush(open_file_t *f) {
    if (f->fs != FS_FAT || !f->dirty || !f->wbuf) return;
    fat32_write(f->path, f->wbuf, f->size);
    f->dirty = 0;
}

int vfs_open(const char *path) {
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        struct fat_dirent e;
        if (!fat32_stat(sub, &e)) return -1;
        int fd = alloc_slot();
        if (fd < 0) return -1;
        open_file_t *f = &open_files[fd];
        f->used = 1;
        f->fs = FS_FAT;
        f->pos = 0;
        vstrcpy(f->path, sub, FAT_PATH_MAX);
        f->size = e.size;
        f->wbuf = NULL;
        f->wcap = 0;
        f->dirty = 0;
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
    f->wbuf = NULL;
    f->dirty = 0;
    return fd;
}

int32_t vfs_read(int fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_FAT) {
        // An unflushed write buffer is the current truth for this file.
        if (f->wbuf) {
            if (f->pos >= f->size) return 0;
            uint32_t n = f->size - f->pos;
            if (n > len) n = len;
            uint8_t *out = (uint8_t *)buf;
            for (uint32_t i = 0; i < n; i++) out[i] = f->wbuf[f->pos + i];
            f->pos += n;
            return (int32_t)n;
        }
        long n = fat32_read(f->path, f->pos, buf, len);
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

    if (f->fs == FS_FAT) {
        if (len == 0) return 0;
        if (!fat_buf_ensure(f, f->pos + len)) return -1;
        const uint8_t *src = (const uint8_t *)buf;
        for (uint32_t i = 0; i < len; i++) f->wbuf[f->pos + i] = src[i];
        f->pos += len;
        if (f->pos > f->size) f->size = f->pos;
        f->dirty = 1;
        return (int32_t)len;
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
    if (f->used && f->fs == FS_FAT) {
        fat_flush(f);                 // the write actually reaches the stick here
        if (f->wbuf) { kfree(f->wbuf); f->wbuf = NULL; f->wcap = 0; }
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
        case VFS_SEEK_END: base = (int64_t)(f->fs == FS_FAT ? f->size : f->inode.i_size); break;
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

    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        struct fat_dirent e;
        if (!fat32_stat(sub, &e)) return -1;
        out->size   = e.size;
        out->inode  = e.cluster;    // no inodes on FAT; the first cluster is the id
        out->is_dir = e.is_dir;
        out->mtime  = 0;            // FAT timestamps are not surfaced yet
        return 0;
    }

    uint32_t ino;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &ino, &inode)) return -1;
    fill_stat(out, ino, &inode);
    return 0;
}

int vfs_truncate(const char *path, uint32_t length) {
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        // Truncation on the stick is expressed through any open fd on the file;
        // with none open, rewrite it at the new length.
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            open_file_t *f = &open_files[i];
            if (f->used && f->fs == FS_FAT && vfs_streq(f->path, sub))
                return vfs_ftruncate(i, length);
        }
        struct fat_dirent e;
        if (!fat32_stat(sub, &e)) return -1;
        if (length == 0) return fat32_write(sub, NULL, 0) < 0 ? -1 : 0;
        if (length >= e.size) return 0;
        uint8_t *tmp = (uint8_t *)kmalloc(length);
        if (!tmp) return -1;
        long got = fat32_read(sub, 0, tmp, length);
        int rc = (got < 0 || fat32_write(sub, tmp, length) < 0) ? -1 : 0;
        kfree(tmp);
        return rc;
    }

    uint32_t ino;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &ino, &inode)) return -1;
    if (!ext2_truncate(ino, length)) return -1;

    // Any fd already open on this inode must see the new size, and its cursor
    // must not be left dangling past the end.
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].used && open_files[i].inode_num == ino) {
            ext2_get_inode(ino, &open_files[i].inode);
            if (open_files[i].pos > length) open_files[i].pos = length;
        }
    }
    return 0;
}

int vfs_ftruncate(int fd, uint32_t length) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !open_files[fd].used) return -1;
    open_file_t *f = &open_files[fd];

    if (f->fs == FS_FAT) {
        // Shrinking (including the truncate-to-0 that fopen("w") does) only has
        // to move the size; growing zero-fills through the buffer.
        if (length > f->size) {
            if (!fat_buf_ensure(f, length)) return -1;
            for (uint32_t i = f->size; i < length; i++) f->wbuf[i] = 0;
        } else if (!fat_buf_ensure(f, length ? length : 1)) {
            return -1;
        }
        f->size = length;
        f->dirty = 1;
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

    if (f->fs == FS_FAT) {
        out->size   = f->size;
        out->inode  = 0;
        out->is_dir = 0;
        out->mtime  = 0;
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
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        struct fat_dirent e;
        if (fat32_stat(sub, &e)) return 0;          // already exists: nothing to do
        return fat32_write(sub, NULL, 0) < 0 ? -1 : 0;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_create(pino, leaf, 0, NULL) ? 0 : -1;
}

int vfs_mkdir(const char *path) {
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        return fat32_mkdir(sub) < 0 ? -1 : 0;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_create(pino, leaf, 1, NULL) ? 0 : -1;
}

int vfs_unlink(const char *path) {
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return -1;
        // Drop any cached write buffer first: flushing it afterwards would
        // recreate the file we were asked to delete.
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            open_file_t *f = &open_files[i];
            if (f->used && f->fs == FS_FAT && vfs_streq(f->path, sub)) f->dirty = 0;
        }
        return fat32_unlink(sub) < 0 ? -1 : 0;
    }

    char parent[256], leaf[256];
    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) return -1;
    uint32_t pino;
    if (!lookup_dir_inode(parent, &pino)) return -1;
    return ext2_unlink(pino, leaf) ? 0 : -1;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    const char *osub = usb_subpath(oldpath);
    const char *nsub = usb_subpath(newpath);
    // Renaming ACROSS the mount boundary would mean copying data between two
    // filesystems, which rename() does not do (POSIX calls this EXDEV); use cp.
    if ((osub != NULL) != (nsub != NULL)) return -1;
    if (osub && nsub) {
        if (!usb_ready()) return -1;
        return fat32_rename(osub, nsub) < 0 ? -1 : 0;
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

static void list_cb(const char *name, uint8_t name_len, int is_dir, void *userdata) {
    list_ctx_t *ctx = (list_ctx_t *)userdata;
    uint32_t need = name_len + (is_dir ? 1 : 0) + 1;
    if (ctx->written + need > ctx->cap) return;
    for (uint8_t i = 0; i < name_len; i++) {
        ctx->buf[ctx->written++] = name[i];
    }
    if (is_dir) {
        ctx->buf[ctx->written++] = '/';
    }
    ctx->buf[ctx->written++] = '\n';
}

static void fat_list_cb(const struct fat_dirent *e, void *userdata) {
    uint8_t n = 0;
    while (e->name[n]) n++;
    list_cb(e->name, n, e->is_dir, userdata);
}

uint32_t vfs_list_dir(const char *path, char *out_buf, uint32_t out_buf_len) {
    const char *sub = usb_subpath(path);
    if (sub) {
        if (!usb_ready()) return 0;
        list_ctx_t ctx = { out_buf, out_buf_len, 0 };
        fat32_list(sub, fat_list_cb, &ctx);
        return ctx.written;
    }

    uint32_t inode_num;
    ext2_inode_t inode;
    if (!ext2_lookup(path, &inode_num, &inode)) return 0;

    list_ctx_t ctx = { out_buf, out_buf_len, 0 };
    ext2_iterate_dir(&inode, list_cb, &ctx);

    // Show the mount point when listing the root, so the stick is discoverable
    // without knowing it is there. Only when a USB disk is actually plugged in;
    // usb_disk_present() is a cheap counter, no I/O.
    if (path[0] == '/' && path[1] == '\0' && usb_disk_present())
        list_cb("usb", 3, 1, &ctx);

    return ctx.written;
}
