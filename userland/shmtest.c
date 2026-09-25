/* Shared memory: that two programs really see the same bytes, that a segment
 * outlives the program that made it, that letting go of one gives the memory
 * back, and that read-only means read-only.
 *
 * The test with teeth is the fourth. A separate program, with its own page
 * tables, writes a megabyte into the segment and the parent checks every
 * byte. Nothing copies it; if the frames were not literally the same ones,
 * the parent would read zeros.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <thread.h>
#include <unistd.h>
#include <sys/shm.h>
#include "xyuos_syscall.h"

#define KEY        4242
#define SEG_SIZE   (2 * 1024 * 1024)
#define MARK_OFF   0
#define ANSWER_OFF 64
#define BULK_OFF   4096
#define BULK_LEN   (1024 * 1024)

static volatile sig_atomic_t child_done;
static void on_usr1(int sig) { (void)sig; child_done = 1; }

static jmp_buf escape;
static void on_segv(int sig) { (void)sig; longjmp(escape, 1); }

static unsigned long free_frames(void) {
    struct si_mem m;
    if (xyuos_sysinfo(SI_MEM, &m, sizeof m) < 0) return 0;
    return m.free_frames;
}

int main(void) {
    static volatile int bad = 0;

    /* --- 1. make one, and look at what came back --------------------------- */
    int id = shmget(KEY, SEG_SIZE, IPC_CREAT);
    if (id < 0) { printf("FAILED: shmget\n"); return 1; }
    unsigned long size = shmsize(id);
    if (size != SEG_SIZE) { printf("FAILED: size is %lu, not %d\n", size, SEG_SIZE); bad++; }
    else printf("ok: a %lu-byte segment, id %d\n", size, id);

    if (shmget(KEY, SEG_SIZE, IPC_CREAT | IPC_EXCL) >= 0) {
        printf("FAILED: IPC_EXCL made a second one under the same key\n"); bad++;
    } else printf("ok: IPC_EXCL refuses a key that is taken\n");

    if (shmget(9999, 4096, 0) >= 0) {
        printf("FAILED: found a segment that was never made\n"); bad++;
    } else printf("ok: no such key, and no IPC_CREAT\n");

    /* --- 2. the same memory twice in one program --------------------------- */
    unsigned char *a = (unsigned char *)shmat(id, NULL, 0);
    if (a == (unsigned char *)-1) { printf("FAILED: shmat\n"); return 1; }
    /* It must be zero to start with: the frames were somebody else's once. */
    int dirty = 0;
    for (int i = 0; i < 65536; i++) if (a[i]) dirty++;
    if (dirty) { printf("FAILED: %d of the first 65536 bytes were not zero\n", dirty); bad++; }
    else printf("ok: it arrives zeroed\n");

    strcpy((char *)(a + MARK_OFF), "from the parent");

    unsigned char *b = (unsigned char *)shmat(id, NULL, 0);
    if (b == (unsigned char *)-1) { printf("FAILED: second shmat\n"); bad++; }
    else if (b == a) { printf("FAILED: the second attach came back at the same address\n"); bad++; }
    else if (strcmp((char *)(b + MARK_OFF), "from the parent") != 0) {
        printf("FAILED: the second view does not show what the first wrote\n"); bad++;
    } else {
        b[ANSWER_OFF] = 'X';
        if (a[ANSWER_OFF] != 'X') { printf("FAILED: writing through one view is not visible in the other\n"); bad++; }
        else printf("ok: two views at %p and %p, one memory\n", (void *)a, (void *)b);
        a[ANSWER_OFF] = 0;
        shmdt(b);
    }

    /* --- 3. letting go of a view does not take the memory with it ---------- */
    if (strcmp((char *)(a + MARK_OFF), "from the parent") != 0) {
        printf("FAILED: detaching one view damaged the other\n"); bad++;
    } else printf("ok: one view let go, the other unharmed\n");

    /* --- 4. another program, its own page tables, the same bytes ----------- */
    signal(SIGUSR1, on_usr1);
    {
        char skey[16], spid[16];
        snprintf(skey, sizeof skey, "%d", KEY);
        snprintf(spid, sizeof spid, "%d", thread_self());
        char *const argv[] = { (char *)"/bin/shmchild", skey, spid };
        int pid = spawn("/bin/shmchild", argv, 3);
        if (pid < 0) { printf("FAILED: cannot start shmchild\n"); bad++; }
        else {
            /* Wait for its signal, not for its exit: the point is that the
             * memory is shared while both are alive. */
            for (int i = 0; i < 100 && !child_done; i++) sleep_ms(100);
            if (!child_done) { printf("FAILED: the child never signalled\n"); bad++; }

            if (strcmp((char *)(a + ANSWER_OFF), "from the child") != 0) {
                printf("FAILED: the child's answer is not there\n"); bad++;
            } else printf("ok: the child wrote and this program can read it\n");

            int wrong = 0;
            for (int i = 0; i < BULK_LEN; i++)
                if (a[BULK_OFF + i] != (unsigned char)(i * 7 + 3)) wrong++;
            if (wrong) { printf("FAILED: %d of %d bytes differ\n", wrong, BULK_LEN); bad++; }
            else printf("ok: a megabyte written there, read back here, byte for byte\n");

            int code = waitpid(pid);
            if (code != 0) { printf("FAILED: the child exited %d\n", code); bad++; }
        }
    }

    /* --- 5. read-only means read-only -------------------------------------- */
    {
        unsigned char *ro = (unsigned char *)shmat(id, NULL, SHM_RDONLY);
        if (ro == (unsigned char *)-1) { printf("FAILED: read-only shmat\n"); bad++; }
        else {
            if (strcmp((char *)(ro + MARK_OFF), "from the parent") != 0) {
                printf("FAILED: cannot read a read-only view\n"); bad++;
            }
            signal(SIGSEGV, on_segv);
            if (setjmp(escape) == 0) {
                ro[0] = 'z';
                printf("FAILED: wrote to a read-only segment\n"); bad++;
            } else {
                printf("ok: read-only, and writing to it is stopped\n");
            }
            sigset_t s;
            sigemptyset(&s); sigaddset(&s, SIGSEGV);
            sigprocmask(SIG_UNBLOCK, &s, NULL);
            signal(SIGSEGV, SIG_DFL);
            shmdt(ro);
        }
    }

    /* --- 6. removing it, and the key going with it ------------------------- */
    if (shmctl(id, IPC_RMID, NULL) != 0) { printf("FAILED: shmctl IPC_RMID\n"); bad++; }
    if (shmget(KEY, 0, 0) >= 0) {
        printf("FAILED: the key still finds a removed segment\n"); bad++;
    } else printf("ok: removed, and the key no longer finds it\n");
    /* Still mapped here, and still readable: it goes when the last one lets
     * go, not when it is marked. */
    if (strcmp((char *)(a + MARK_OFF), "from the parent") != 0) {
        printf("FAILED: a removed segment stopped working while still mapped\n"); bad++;
    } else printf("ok: marked for removal, still there for whoever holds it\n");
    shmdt(a);

    /* --- 7. and the memory really comes back ------------------------------- */
    {
        unsigned long before = free_frames();
        for (int round = 0; round < 3; round++) {
            int t = shmget(IPC_PRIVATE, 4 * 1024 * 1024, IPC_CREAT);
            if (t < 0) { printf("FAILED: round %d could not get 4 MiB\n", round); bad++; break; }
            void *p = shmat(t, NULL, 0);
            if (p == (void *)-1) { printf("FAILED: round %d could not attach\n", round); bad++; break; }
            memset(p, 0xA5, 4 * 1024 * 1024);
            shmdt(p);
            shmctl(t, IPC_RMID, NULL);
        }
        unsigned long after = free_frames();
        /* Not exactly equal: other programs are running. Within a megabyte
         * is the difference between "given back" and "leaked twelve". */
        long drift = (long)before - (long)after;
        if (drift > 256 || drift < -256) {
            printf("FAILED: %ld frames did not come back after three 4 MiB rounds\n", drift);
            bad++;
        } else {
            printf("ok: three 4 MiB segments taken and given back (%ld frames drift)\n", drift);
        }
    }

    /* --- 8. a program that walks out still holding one ---------------------
     *
     * Nothing in the child lets go. The only thing that can is the kernel
     * taking its address space apart -- and if it got that wrong the frames
     * would either leak (nobody ever frees them) or, far worse, be handed
     * back to the allocator while they are still somebody's. */
    {
        unsigned long before = free_frames();
        int t = shmget(4243, 4 * 1024 * 1024, IPC_CREAT);
        if (t < 0) { printf("FAILED: cannot make a segment for the holder\n"); bad++; }
        else {
            char skey[16], spid[16];
            snprintf(skey, sizeof skey, "%d", 4243);
            snprintf(spid, sizeof spid, "%d", thread_self());
            char *const argv[] = { (char *)"/bin/shmchild", skey, spid,
                                   (char *)"hold" };
            int pid = spawn("/bin/shmchild", argv, 4);
            if (pid < 0) { printf("FAILED: cannot start the holder\n"); bad++; }
            else if (waitpid(pid) != 0) { printf("FAILED: the holder failed\n"); bad++; }
            else {
                /* Marked for removal with nobody left holding it: if the
                 * child's exit released its hold, this frees it now. */
                shmctl(t, IPC_RMID, NULL);
                unsigned long after = free_frames();
                long drift = (long)before - (long)after;
                if (drift > 256 || drift < -256) {
                    printf("FAILED: a program that exited while attached kept %ld frames\n",
                           drift);
                    bad++;
                } else {
                    printf("ok: a program exited still attached, and its hold went with it\n");
                }
            }
        }
    }

    printf(bad ? "FAILED\n" : "all good\n");
    return bad ? 1 : 0;
}
