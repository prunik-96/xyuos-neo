/* Threads.
 *
 * A thread here is a process that shares another's memory: the same heap, the
 * same mappings, the same page tables, and nothing else. It gets its own user
 * stack -- a mapping of a megabyte, built a page at a time as it is used --
 * and its own place in the scheduler.
 *
 * Deliberately not pthreads. The names are shorter, there are no attribute
 * objects, and a thread is named by a small integer rather than an opaque
 * handle, because everything underneath is a process id already. A pthread
 * layer can sit on top of this later without any of it changing.
 *
 * The thread function is passed its argument and its return is the thread's
 * end: falling off it is the same as calling thread_exit(0).
 */
#ifndef XYUOS_THREAD_H
#define XYUOS_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start one. Returns its id, or -1 if there was no slot or no memory. */
int  thread_create(void (*fn)(void *), void *arg);

/* End this one. Never returns. Calling it on the program's first thread ends
 * the program, which is what was meant. */
void thread_exit(int code);

/* Wait for that one to end. Returns 0 when it has, or once it already had;
 * -1 if the id is not a thread of this program. */
int  thread_join(int tid);

/* This thread's id. */
int  thread_self(void);

/* --- keeping two threads out of each other's way --------------------------
 *
 * A lock held by nobody is 0. Taking it is an atomic exchange, and a thread
 * that finds it held spins -- there is no queue and no sleeping, so a lock
 * should be held across a few instructions and not across a fetch.
 *
 * Spinning is honest on a machine with more than one core and merely slow on
 * one, where the waiter holds its slice until the timer takes it away. What
 * it is not is a lie: the memory ordering comes from the compiler's atomic
 * builtins, not from hoping.
 */
typedef struct { volatile int held; } mutex_t;

#define MUTEX_INIT { 0 }

static inline void mutex_init(mutex_t *m) { m->held = 0; }

static inline void mutex_lock(mutex_t *m) {
    while (__sync_lock_test_and_set(&m->held, 1)) {
        while (m->held) { __asm__ volatile ("pause" ::: "memory"); }
    }
}

static inline int mutex_try_lock(mutex_t *m) {
    return __sync_lock_test_and_set(&m->held, 1) == 0;
}

static inline void mutex_unlock(mutex_t *m) {
    __sync_lock_release(&m->held);
}

#ifdef __cplusplus
}
#endif

#endif
