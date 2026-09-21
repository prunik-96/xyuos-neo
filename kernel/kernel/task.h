#ifndef TASK_H
#define TASK_H

#include <stdint.h>

typedef enum { TASK_READY, TASK_TERMINATED } task_state_t;

typedef struct task {
    uint64_t rsp;
    uint8_t *stack_base;
    struct task *next;
    task_state_t state;
    char name[16];
} task_t;

void sched_init(void);
task_t *task_create(const char *name, void (*entry)(void));
task_t *task_create_sized(const char *name, void (*entry)(void), int stack_bytes);
void yield(void);
void task_exit(void);

/* --- coroutines ----------------------------------------------------------
 * yield() hands the processor to whoever is next in the ring, which is what a
 * set of equal peers wants. These two are for the other shape: one task runs
 * another for a while and gets it back. task_resume() returns when the task it
 * started calls task_yield_back(), and the task carries on from there the next
 * time it is resumed. That is what lets a long blocking routine be written as
 * straight-line code and still be interruptible -- it does not have to become
 * a state machine to be put down and picked up again. */
void task_resume(task_t *t);
void task_yield_back(void);

#endif
