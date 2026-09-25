#ifndef XYUOS_SYSCALL_H
#define XYUOS_SYSCALL_H

#define SYS_WRITE     1
#define SYS_EXIT      2
#define SYS_OPEN      3
#define SYS_READ      4
#define SYS_CLOSE     5
#define SYS_LISTDIR   6
#define SYS_READKEY   7
#define SYS_HYPER     8
#define SYS_CREATE    9
#define SYS_WRITEFILE 10
#define SYS_UNLINK    11
#define SYS_MKDIR     12
#define SYS_RENAME    13
#define SYS_READEVENT 14
#define SYS_TERMSIZE  15
#define SYS_SEEK      16
#define SYS_STAT      17
#define SYS_TRUNCATE  18
#define SYS_SBRK      19
#define SYS_SPAWN     20
#define SYS_WAIT      21
#define SYS_TTYCTL    22

#define TTY_CLEAR       0
#define TTY_MOVE        1
#define TTY_SETCOLOR    2
#define TTY_RESETCOLOR  3
#define TTY_ERASELINE   4
#define TTY_SIZE        5
#define TTY_GETCUR      6

#define SYS_SPAWN2    23
#define SYS_READSTD   24
#define SYS_SYSINFO   25
#define SYS_SLEEP     26
#define SYS_KILL      27
#define SYS_PIPE      28
#define SYS_PIPECLOSE 29
#define SYS_POWER     30
#define SYS_TIME      31
#define SYS_USBFS     32
#define SYS_NET       33
#define SYS_GFX       34
#define SYS_POLLEVENT 35
/* SYS_POLLEVENT ops (must match kernel syscall.h). */
#define POLLEV_NEXT   0   /* a1=0: take the next event (the default) */
#define POLLEV_KEYUP  1   /* a1=1: a2 = 1 to also receive key-up + modifier keys */
#define SYS_UPTIME    36
#define SYS_SMP       37
#define SYS_POLLMOUSE 38
#define SYS_FONT      39
#define SYS_PROCCTL   40
#define SYS_MSGBOX    41
#define SYS_AUDIO     42
#define SYS_SPAWNWIN  43
#define SYS_DRAG      44

#define DRAG_BEGIN  0
#define DRAG_TAKE   1
#define DRAG_CANCEL 2

/* Mirrors struct winspawn in the kernel's syscall.h. */
struct winspawn {
    const char *path;
    const char *arg;
};

#define AU_INFO   0
#define AU_WRITE  1
#define AU_QUEUED 2
#define AU_STOP   3
#define AU_VOLUME 4

/* Mirrors struct msgbox_req in the kernel's syscall.h. */
struct msgbox_req {
    int         kind;
    const char *code;
    const char *title;
    const char *text;
    const char *detail;
};

#define PC_KILL   0
#define PC_STOP   1
#define PC_CONT   2

#define FONT_INFO  0
#define FONT_ATLAS 1

// SYS_GFX ops (must match kernel syscall.h).
#define GFX_BLIT 0
#define GFX_INFO 1
#define GFX_END  2

#define POWER_REBOOT 0
#define POWER_OFF    1
#define POWER_SLEEP  2

#define SI_MEM    0
#define SI_PROCS  1
#define SYS_SETTING   45  /* op=a1, key=a2, value=a3 -> the value, or -1 */
#define SYS_VM        46  // op=a1: 0 map, 1 unmap, 2 protect

// What a program may do with a piece of memory. The same three bits libc
// spells PROT_READ / PROT_WRITE / PROT_EXEC.
// a1 = op | (protection << 8), because the dispatcher has three arguments
// and protect needs to say four things.
#define VM_OP_MAP      0   // a2 = length                -> address, or 0
#define VM_OP_UNMAP    1   // a2 = address, a3 = length  -> 0, or -1
#define VM_OP_PROTECT  2   // a2 = address, a3 = length  -> 0, or -1

/* SYS_SETTING operations. */
#define SETOP_GET        0   /* a2 = SET_*                    -> value        */
#define SETOP_SET        1   /* a2 = SET_*, a3 = value        -> the value set*/
#define SETOP_THEME_NAME 2   /* a2 = theme index, a3 = char *buf (>= 32)      */
#define SETOP_PALETTE    3   /* a2 = struct ui_palette *, a3 = its size       */
// The colours a program should draw with, from the current theme.
struct ui_palette {
    unsigned int win, panel, alt, text, dim, line, edge;
    unsigned int accent, accent2, sel, hot, btn, btndn;
    unsigned int bar, bar2, warn, good;
};


/* The settings themselves. */
#define SET_THEME        0   /* 0 .. SET_THEME_COUNT-1        */
#define SET_THEME_COUNT  1   /* read-only                     */
#define SET_KEY_DELAY    2   /* auto-repeat delay, ms         */
#define SET_KEY_RATE     3   /* auto-repeat interval, ms      */
#define SET_MOUSE_SPEED  4   /* pointer speed, percent        */
#define SET_DBLCLICK     5   /* double-click window, ms       */

#define SI_DEVICES 3
#define SI_UNAME  2

/* Mirrors the kernel's definitions in arch/x86_64/syscall.h. */
struct si_mem {
    unsigned long page_size;
    unsigned long total_frames;
    unsigned long free_frames;
};

#define SI_NAME_MAX 32

/* --- the device list (SI_DEVICES), mirroring the kernel --- */
// Categories, in the order a device manager should group them.
#define DEVC_CPU      0
#define DEVC_MEMORY   1
#define DEVC_DISPLAY  2
#define DEVC_STORAGE  3
#define DEVC_NETWORK  4
#define DEVC_AUDIO    5
#define DEVC_USB      6
#define DEVC_INPUT    7
#define DEVC_BRIDGE   8
#define DEVC_OTHER    9
#define DEVC_COUNT   10

struct si_dev {
    unsigned char cat;                 // DEVC_*
    unsigned char bus, slot, func;     // PCI address, or 0xFF when not on PCI
    unsigned short vendor, device;     // PCI ids, 0 when not on PCI
    char name[52];                     // what it is, in words
    char driver[16];                   // the driver bound to it, "" if none
    char status[20];                   // what that driver has to say
};

struct si_proc {
    int pid;
    int parent_pid;
    int state;
    int pane;
    int stopped;        // 1 if suspended by the task manager
    unsigned long long ticks;   // timer ticks spent on the CPU
    char name[SI_NAME_MAX];
};

/* Mirrors struct spawn_req in kernel/arch/x86_64/syscall.h -- keep in step. */
struct spawn_req {
    const char *path;
    const char *const *argv;
    int argc;
    const char *in_path;
    const char *out_path;
    int append;
    int in_pipe;     /* pipe index for stdin, or -1 */
    int out_pipe;    /* pipe index for stdout, or -1 */
};

/* Mirrors struct net_req in kernel/arch/x86_64/syscall.h -- keep in step. */
struct net_req {
    int op;
    const char *a;
    const char *b;
    void *buf;
    unsigned int len;
    unsigned int arg;
    const char *hdr;     /* extra request headers, CRLF-terminated, or 0 */
    unsigned int hdrlen;
    int result;
};
#define NET_UP      0
#define NET_CONFIG  1
#define NET_PING    2
#define NET_RESOLVE 3
#define NET_HTTPGET 4
#define NET_STATUS  5
#define NET_LOG     6
#define NET_FSTART  7
#define NET_FPOLL   8
#define NET_FTAKE   9
#define NET_FSLOTS  10   // result = how many fetches may run at once
#define NET_FCANCEL 11   // arg = slot; give it up
/* Set in `arg` alongside the port to fetch over TLS. A flag rather than a
 * separate op, because everything else about the request is identical. */
#define NET_HTTPGET_TLS 0x10000
/* Return the whole response -- status line, headers, blank line, body --
 * instead of the body alone. What a browser needs to follow a redirect. */
#define NET_HTTPGET_POST 0x20000000u  /* there is a body: POST it */
#define NET_HTTPGET_RAW 0x20000


/* struct net_info is defined in <unistd.h> (the public API surface). */

/* NOTE: this table is a SECOND copy of the numbers in
 * kernel/arch/x86_64/syscall.h -- userland cannot include kernel headers.
 * The two must be kept in step, and new numbers must be APPENDED, never
 * inserted, or the kernel and libc will disagree about what a number means. */

/* SYSCALL destroys RCX (the return address) and R11 (the flags), and the
 * kernel gives everything else back -- syscall_entry.S saves and restores the
 * argument registers around its call into C.
 *
 * That was not always true, and the way it failed is worth keeping: the entry
 * code used RDI/RSI/RDX to marshal arguments and left them destroyed, while
 * GCC compiled against the standard rule that input registers survive. Two
 * syscalls in a row differing in one argument would reload only that one and
 * reuse the kernel's leftovers for the rest. No crash, just wrong values, in
 * whichever program happened to make consecutive calls.
 *
 * R8-R10 stay in the clobber list: it costs nothing and this header should not
 * be the only thing standing between a future scratch register and the same
 * class of bug. */
static inline long xyuos_syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "r8", "r9", "r10", "memory"
    );
    return ret;
}

#endif
