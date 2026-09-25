#include "task.h"
#include "../mm/heap.h"
#include <stddef.h>

#define TASK_STACK_SIZE (16 * 1024)

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static task_t main_task;
static task_t *current_task = NULL;

static void strcopy(char *dst, const char *src, int max) {
    int i = 0;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void sched_init(void) {
    main_task.rsp = 0;
    main_task.stack_base = NULL;
    main_task.state = TASK_READY;
    main_task.next = &main_task;
    strcopy(main_task.name, "main", sizeof(main_task.name));
    current_task = &main_task;
}

task_t *task_create(const char *name, void (*entry)(void)) {
    return task_create_sized(name, entry, TASK_STACK_SIZE);
}

task_t *task_create_sized(const char *name, void (*entry)(void), int stack_bytes) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    uint8_t *stack = (uint8_t *)kmalloc(stack_bytes);

    uint64_t *sp = (uint64_t *)(stack + stack_bytes);
    *--sp = (uint64_t)entry; // popped by context_switch's `ret`
    *--sp = 0; // r15
    *--sp = 0; // r14
    *--sp = 0; // r13
    *--sp = 0; // r12
    *--sp = 0; // rbp
    *--sp = 0; // rbx

    t->rsp = (uint64_t)sp;
    t->stack_base = stack;
    t->state = TASK_READY;
    strcopy(t->name, name, sizeof(t->name));

    // insert right after current_task in the ring
    t->next = current_task->next;
    current_task->next = t;

    return t;
}

task_t *task_create_coroutine(const char *name, void (*entry)(void),
                              int stack_bytes) {
    task_t *t = task_create_sized(name, entry, stack_bytes);
    if (t) t->state = TASK_COROUTINE;
    return t;
}

void yield(void) {
    task_t *old = current_task;
    task_t *next = old->next;

    while (next->state != TASK_READY && next != old) {
        next = next->next;
    }
    if (next == old && old->state != TASK_READY) {
        for (;;) { __asm__ volatile ("hlt"); }
    }
    if (next == old) {
        return;
    }

    current_task = next;
    context_switch(&old->rsp, next->rsp);
}

/* Who to hand the processor back to. A single slot is enough because these
 * pairs never nest: the resumed task does network waiting, not resuming. */
static task_t *resumer = NULL;

void task_resume(task_t *t) {
    if (!t || t->state == TASK_TERMINATED || t == current_task) return;
    task_t *me = current_task;
    task_t *saved = resumer;
    resumer = me;
    current_task = t;
    context_switch(&me->rsp, t->rsp);
    /* Back again: the task we started has handed the processor over. */
    current_task = me;
    resumer = saved;
}

void task_yield_back(void) {
    task_t *back = resumer;
    if (!back) return;
    task_t *me = current_task;
    current_task = back;
    context_switch(&me->rsp, back->rsp);
}

void task_exit(void) {
    current_task->state = TASK_TERMINATED;
    for (;;) {
        yield();
    }
}


