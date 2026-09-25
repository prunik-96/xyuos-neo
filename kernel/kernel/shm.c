// Shared memory. See shm.h for what it is and why it is shaped this way.

#include "shm.h"
#include "process.h"
#include "kio.h"
#include "../mm/vmm.h"
#include "../mm/paging.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"

#define PAGE 4096ULL

struct shm_seg {
    int       used;
    int       id;          // what userland holds; never reused while live
    int       key;         // 0 for a private segment, which nobody can name
    int       removed;     // marked for removal: no new attach, go when empty
    uint32_t  npages;
    uint64_t *frames;      // npages physical addresses, not necessarily in order
    int       maps;        // how many address spaces have it mapped
};

static struct shm_seg segs[SHM_SEGMENTS];
static int next_id = 1;

static struct shm_seg *by_id(int id) {
    if (id <= 0) return 0;
    for (int i = 0; i < SHM_SEGMENTS; i++)
        if (segs[i].used && segs[i].id == id) return &segs[i];
    return 0;
}

static struct shm_seg *by_key(int key) {
    if (key == IPC_PRIVATE) return 0;
    for (int i = 0; i < SHM_SEGMENTS; i++)
        if (segs[i].used && !segs[i].removed && segs[i].key == key)
            return &segs[i];
    return 0;
}

// Give the frames back and let the slot go. Only ever called with maps == 0.
static void seg_free(struct shm_seg *s) {
    for (uint32_t i = 0; i < s->npages; i++)
        if (s->frames[i]) pmm_free_frame(s->frames[i]);
    kfree(s->frames);
    s->frames = 0;
    s->npages = 0;
    s->used = 0;
    s->id = 0;
    s->key = 0;
    s->removed = 0;
}

int shm_get(int key, uint64_t size, int flags) {
    struct shm_seg *have = by_key(key);
    if (have) {
        // Asking to create one that is already there is an error only if the
        // program said it wanted to be the one creating it.
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -1;
        if (size > (uint64_t)have->npages * PAGE) return -1;   // too small
        return have->id;
    }
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -1;
    if (size == 0) return -1;

    uint64_t npages = (size + PAGE - 1) / PAGE;
    if (npages > SHM_MAX_PAGES) return -1;

    struct shm_seg *s = 0;
    for (int i = 0; i < SHM_SEGMENTS; i++)
        if (!segs[i].used) { s = &segs[i]; break; }
    if (!s) return -1;

    s->frames = (uint64_t *)kmalloc((uint64_t)npages * sizeof(uint64_t));
    if (!s->frames) return -1;
    for (uint64_t i = 0; i < npages; i++) s->frames[i] = 0;

    // Built in full here rather than on demand. A page of shared memory has
    // no single owner to fault it in, and two processes touching the same
    // missing page at once is a race worth not having.
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t f = pmm_alloc_frame();
        if (!f) {
            for (uint64_t k = 0; k < i; k++) pmm_free_frame(s->frames[k]);
            kfree(s->frames);
            s->frames = 0;
            return -1;
        }
        // Zeroed: the frame was somebody else's a moment ago, and shared
        // memory is read by a program that did not write it.
        uint8_t *q = (uint8_t *)(uintptr_t)f;
        for (uint64_t b = 0; b < PAGE; b++) q[b] = 0;
        s->frames[i] = f;
    }

    s->used = 1;
    s->id = next_id++;
    s->key = key;
    s->removed = 0;
    s->npages = (uint32_t)npages;
    s->maps = 0;
    return s->id;
}

uint64_t shm_attach(int id, int flags) {
    process_t *p = process_current();
    struct shm_seg *s = by_id(id);
    if (!p || !p->as || !s) return 0;
    // A removed segment is on its way out; letting somebody new in would
    // mean it never left.
    if (s->removed) return 0;

    uint32_t prot = VM_READ | ((flags & SHM_RDONLY) ? 0 : VM_WRITE);

    uint64_t len = (uint64_t)s->npages * PAGE;
    uint64_t at = vmm_reserve(len, prot, id);
    if (!at) return 0;

    for (uint32_t i = 0; i < s->npages; i++) {
        if (paging_map(p->pml4, at + (uint64_t)i * PAGE, s->frames[i],
                       vmm_page_flags(prot)) != 0) {
            // Undo as far as we got: a half-mapped segment would be a hole in
            // the middle of somebody's array.
            paging_detach(p->pml4, at, (uint64_t)i * PAGE);
            vmm_release(at);
            return 0;
        }
    }
    s->maps++;
    return at;
}

int shm_detach(uint64_t addr) {
    // vmm_munmap knows it is a shared region and calls shm_dropped for us.
    return vmm_munmap_shared(addr);
}

int shm_ctl(int id, int cmd) {
    struct shm_seg *s = by_id(id);
    if (!s) return -1;
    if (cmd != IPC_RMID) return -1;
    s->removed = 1;
    // The name goes at once, so a program can put a fresh segment under the
    // same key immediately; the memory goes when the last mapping does.
    s->key = 0;
    if (s->maps == 0) seg_free(s);
    return 0;
}

uint64_t shm_size(int id) {
    struct shm_seg *s = by_id(id);
    return s ? (uint64_t)s->npages * PAGE : 0;
}

void shm_dropped(int id) {
    struct shm_seg *s = by_id(id);
    if (!s) return;
    if (s->maps > 0) s->maps--;
    if (s->maps == 0 && s->removed) seg_free(s);
}
