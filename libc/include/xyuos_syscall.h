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
#define AU_TITLE  5

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
#define PC_ALIVE  3

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

#define SYS_THREAD    47  // op=a1: 0 create, 1 exit, 2 join, 3 self

// a2/a3 depend on the op:
//   CREATE  a2 = entry, a3 = argument  -> the new thread id, or -1
//   EXIT    a2 = exit code             -> does not return
//   JOIN    a2 = thread id             -> 0 once it has ended, or -1
//   SELF                               -> this thread's id
#define THREAD_OP_CREATE 0
#define THREAD_OP_EXIT   1
#define THREAD_OP_JOIN   2
#define THREAD_OP_SELF   3

// Signals. op=a1; see signal.h for what a2 and a3 mean for each.
#define SYS_SIGNAL    48
#define SIGOP_HANDLER 0
#define SIGOP_SEND    1
#define SIGOP_RETURN  2
#define SIGOP_TRAMP   3
#define SIGOP_ALARM   4
#define SIGOP_MASK    5

#define SIGMASK_SET     0
#define SIGMASK_BLOCK   1
#define SIGMASK_UNBLOCK 2

// Shared memory. op=a1; see sys/shm.h.
#define SYS_SHM       49

// A notice on the desktop: a1 = title, a2 = text.
#define SYS_NOTIFY    50

// A directory listing with what a file manager shows: SYS_READDIR fills an
// array of these. a1 = path, a2 = struct xdirent *, a3 = how many fit ->
// how many there are (only that many written), or -1.
#define SYS_READDIR   51
struct xdirent {
    char name[256];              // UTF-8
    unsigned long long size;     // bytes; 0 for a directory
    long long mtime;             // seconds since 1970, 0 if not known
    unsigned int is_dir;
    unsigned int flags;          // XD_MOUNT: a drive's mount point in "/"
};
#define XD_MOUNT 1

// The drives: the system disk and every volume on a USB drive.
// a1 = VOLOP_*.
#define SYS_VOLUMES   52
#define VOLOP_LIST  0            // a2 = struct uvol *, a3 = max -> how many
#define VOLOP_EJECT 1            // a2 = index -> 0, or -1
#define VOLOP_SPACE 2            // a2 = index (-1 the system), a3 = struct uvol * -> 0, or -1
struct uvol {
    char mount[16];              // "/" for the system, "/usb", "/usb2", ...
    char label[64];              // the filesystem's own name, may be empty
    char drive[48];              // what the drive calls itself
    char fs[8];                  // "ext2", "FAT32", "exFAT", "NTFS", ...
    unsigned long long total, free;   // bytes; free only after VOLOP_SPACE
    int index;                   // -1 for the system disk
    int usable;                  // its files can be opened
};


// The process's environment: NAME=VALUE strings, a copy of its parent's.
// a1 = ENVOP_GET (a2 buf, a3 room -> the whole length) or ENVOP_SET (a2 the
// new block of NUL-ended strings, a3 its length -> 0).
#define SYS_ENV       54
#define ENVOP_GET 0
#define ENVOP_SET 1

// Sockets (kernel/net/sock.h). a1 = struct sock_req *; the answer is in
// result: a count, a socket, or 0 -- or a negated errno.
#define SYS_SOCKET    56
#define SOCKOP_OPEN     0        // arg = 1 stream, 2 datagram       -> socket
#define SOCKOP_BIND     1        // sid, ip, port
#define SOCKOP_LISTEN   2        // sid, arg = backlog
#define SOCKOP_ACCEPT   3        // sid                              -> socket; ip, port = the peer
#define SOCKOP_CONNECT  4        // sid, ip, port
#define SOCKOP_SEND     5        // sid, buf, len, arg = flags       -> bytes
#define SOCKOP_SENDTO   6        // sid, buf, len, arg = flags, ip, port
#define SOCKOP_RECV     7        // sid, buf, len, arg = flags       -> bytes; ip, port = from
#define SOCKOP_SHUTDOWN 8        // sid, arg = how
#define SOCKOP_CLOSE    9        // sid
#define SOCKOP_NAME     10       // sid, arg = 1 peer / 0 own        -> ip, port
#define SOCKOP_SETOPT   11       // sid, arg = option, val
#define SOCKOP_GETOPT   12       // sid, arg = option                -> value
#define SOCKOP_POLL     13       // buf = struct sock_pollent[len], arg = timeout ms (-1 forever)
struct sock_req {
    int op;
    int sid;
    int arg;
    int val;
    unsigned int ip;             // host order
    unsigned short port;
    unsigned short pad;
    void *buf;
    unsigned int len;
    int result;
};
#ifndef SOCK_POLLENT_DEFINED
#define SOCK_POLLENT_DEFINED
struct sock_pollent { int sid; short events, revents; };
#endif

// The wall clock and the time zone. a1 = CLOCKOP_*.
#define SYS_CLOCK     55
#define CLOCKOP_NOW_MS  0        // -> UTC, milliseconds since 1970
#define CLOCKOP_INFO    1        // a2 = struct clock_info *
#define CLOCKOP_SYNC    2        // a2 = server name or 0 -> 1 set, 0 no answer, -1 no network
#define CLOCKOP_SETZONE 3        // a2 = zone name ("Europe/Moscow") -> 0, or -1
#define CLOCKOP_ZONE    4        // a2 = index, a3 = struct clock_zone * -> 0, or -1 past the end
#define CLOCKOP_OFFSET  5        // a2 = UTC seconds -> seconds east of UTC then
struct clock_info {
    long long utc_ms;            // now
    int  offset;                 // seconds east of UTC now, summer time included
    int  dst;                    // 1 while summer time is on
    int  synced;                 // set from NTP since boot
    int  rtc_known;              // what the board's clock holds is known
    int  rtc_skew;               // the board's clock minus UTC, seconds
    int  delta_ms;               // how far off the last sync found the clock
    int  rtt_ms;
    long long sync_utc;          // when it was last set from NTP (seconds), 0 never
    char zone[40];
    char server[64];
};
struct clock_zone {
    char name[40];               // "Europe/Moscow"
    char ru[64], en[64];         // "Москва, Санкт-Петербург"
    int  std_min;                // standard offset, minutes
    int  rule;                   // summer time: 0 none, 1 European, 2 North American
    int  offset_now;             // seconds, now
};

// The clipboard, shared by every program. a1 = struct clip_req *.
#define SYS_CLIP      53
#define CLIPOP_SET 0             // type, buf, len
#define CLIPOP_GET 1             // buf, len = room -> result = whole length, type
#define CLIPOP_SEQ 2             // -> result = a number that changes on every copy
#define CLIP_NONE  0
#define CLIP_TEXT  1             // UTF-8
#define CLIP_FILES 2             // "copy\n" or "cut\n", then one path a line
struct clip_req {
    int op;
    int type;
    void *buf;
    unsigned int len;
    unsigned int result;
};
#define SHMOP_GET     0
#define SHMOP_ATTACH  1
#define SHMOP_DETACH  2
#define SHMOP_CTL     3
#define SHMOP_SIZE    4

struct shm_req {
    int key;
    int flags;
    unsigned long size;
};

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
#define SET_LANG         6   /* the desktop's language: 0 Russian, 1 English */

#define SI_DEVICES 3

#define SI_CPU    4   // one struct si_cpu
// The processor now: temperature (AMD Ryzen; SENSOR_NONE, -1000000, where it
// cannot be read) and each core's load and effective clock.
struct si_cpu {
    int temp_mc;                 // Tctl, millidegrees Celsius
    int nccd;
    int ccd_mc[8];               // each core die
    int ncpu;
    int load[32];                // percent
    int mhz[32];                 // effective clock, 0 when not known
    int base_mhz;
    char model[64];
};
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
