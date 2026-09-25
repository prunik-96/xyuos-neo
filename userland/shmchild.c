/* The other half of shmtest: a separate program, with its own address space,
 * that attaches the same segment and works in it.
 *
 *   shmchild <key> <parent-pid>
 *
 * Reads what the parent left, writes its own answer over a megabyte of it,
 * and tells the parent with a signal that it is done.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/shm.h>

#define MARK_OFF   0          /* what the parent wrote, at the start */
#define ANSWER_OFF 64
#define BULK_OFF   4096
#define BULK_LEN   (1024 * 1024)

int main(int argc, char **argv) {
    if (argc < 3) { printf("shmchild: need a key and a pid\n"); return 1; }
    int key = (int)strtol(argv[1], NULL, 10);
    int parent = (int)strtol(argv[2], NULL, 10);
    int hold = (argc > 3 && !strcmp(argv[3], "hold"));

    int id = shmget(key, 0, 0);            /* find it; do not create it */
    if (id < 0) { printf("shmchild: no segment under key %d\n", key); return 1; }

    unsigned char *m = (unsigned char *)shmat(id, NULL, 0);
    if (m == (unsigned char *)-1) { printf("shmchild: cannot attach\n"); return 1; }

    /* Attach and walk out without letting go. Somebody has to notice that
     * this program is gone and take its hold off the segment, or the memory
     * is never given back -- which is the whole point of this mode. */
    if (hold) {
        for (int i = 0; i < BULK_LEN; i++) m[BULK_OFF + i] = (unsigned char)i;
        return 0;
    }

    /* What the parent left for us. */
    if (strcmp((char *)(m + MARK_OFF), "from the parent") != 0) {
        printf("shmchild: the parent's mark is not there\n");
        return 2;
    }

    /* Our answer, and a megabyte the parent will check byte by byte. */
    strcpy((char *)(m + ANSWER_OFF), "from the child");
    for (int i = 0; i < BULK_LEN; i++) m[BULK_OFF + i] = (unsigned char)(i * 7 + 3);

    /* Let go before telling: the parent must be able to see everything we
     * wrote whether or not we are still here. */
    shmdt(m);

    kill(parent, SIGUSR1);
    return 0;
}
