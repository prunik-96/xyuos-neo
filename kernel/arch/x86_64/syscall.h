#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

#define SYS_WRITE     1
#define SYS_EXIT      2
#define SYS_OPEN      3
#define SYS_READ      4
#define SYS_CLOSE     5
#define SYS_LISTDIR   6
#define SYS_READKEY   7
#define SYS_HYPER     8
#define SYS_CREATE    9   // create empty regular file at path (a1)
#define SYS_WRITEFILE 10  // write(fd=a1, buf=a2, len=a3) to an open file
#define SYS_UNLINK    11  // remove file/empty dir at path (a1)
#define SYS_MKDIR     12  // create directory at path (a1)
#define SYS_RENAME    13  // rename/move old(a1) -> new(a2)
#define SYS_READEVENT 14  // blocking key event -> packed (mods<<16)|(code<<8)|ascii
#define SYS_TERMSIZE  15  // console dimensions -> packed (cols<<16)|rows
// NOTE: append new numbers at the END. Inserting in the middle renumbers the
// rest, and the Makefile has no header dependencies, so stale .o files would
// silently disagree about what each number means.
#define SYS_SEEK      16  // seek(fd=a1, offset=a2, whence=a3) -> new offset
#define SYS_STAT      17  // stat(path=a1, struct xyuos_stat *out=a2) -> 0/-1
#define SYS_TRUNCATE  18  // ftruncate(fd=a1, length=a2) -> 0/-1
#define SYS_SBRK      19  // sbrk(increment=a1) -> previous break, or -1
#define SYS_SPAWN     20  // spawn(path=a1, argv=a2, argc=a3) -> pid, or -1
#define SYS_WAIT      21  // wait(pid=a1, or -1 for any child) -> exit code, or -1
#define SYS_TTYCTL    22  // control the caller's pane: op=a1, args a2/a3

// SYS_TTYCTL operations. The pane grid is the kernel's; userland reaches it
// only through these, and only ever its own pane.
#define TTY_CLEAR       0  // clear the grid, home the cursor, default colors
#define TTY_MOVE        1  // a2 = row, a3 = col (0-based)
#define TTY_SETCOLOR    2  // a2 = fg, a3 = bg (palette indices)
#define TTY_RESETCOLOR  3
#define TTY_ERASELINE   4  // cursor to end of line
#define TTY_SIZE        5  // -> (cols << 16) | rows
#define TTY_GETCUR      6  // -> (row << 16) | col; line editors need this to
                           //    know where their editable span begins

#define SYS_SPAWN2    23  // spawn with redirection: a1 = struct spawn_req *
#define SYS_READSTD   24  // read(buf=a1, len=a2) from stdin: the redirected
                          // file if there is one, else one keypress
#define SYS_SYSINFO   25  // what=a1, buf=a2, len=a3 -> bytes written, or -1
#define SYS_SLEEP     26  // sleep for a1 milliseconds
#define SYS_KILL      27  // kill(pid=a1) -> 0, or -1 if there is no such process
#define SYS_PIPE      28  // create a pipe -> its index, or -1
#define SYS_PIPECLOSE 29  // pipe_close(idx=a1); clean up a half-wired pipeline
#define SYS_POWER     30  // op=a1: 0 reboot, 1 power off, 2 sleep (halt until key)
#define SYS_TIME      31  // fill struct sys_tm *out (a1) with the RTC wall clock
#define SYS_USBFS     32  // FAT32 on the USB data stick; a1 = struct usbfs_req *
#define SYS_NET       33  // IPv4 stack; a1 = struct net_req *
#define SYS_GFX       34  // raw pixel surface for the caller's pane; a1 = op
#define SYS_POLLEVENT 35  // non-blocking key event -> packed, or -1 if none
#define POLLEV_NEXT   0   // a1=0: take the next event (the default)
#define POLLEV_KEYUP  1   // a1=1: a2 = 1 to also receive key-up + modifier keys
#define SYS_UPTIME    36  // -> milliseconds since boot
#define SYS_SMP       37  // parallel benchmark; a1 = n, a2 = struct smp_result *
#define SYS_POLLMOUSE 38  // non-blocking pointer event -> struct umouse *a1
#define SYS_FONT      39  // the WM's anti-aliased font; a1 = FONT_*
#define SYS_PROCCTL   40  // op=a1 on pid=a2: 0 kill, 1 suspend, 2 resume
#define SYS_MSGBOX    41  // put a modal dialog on the desktop; a1 = req *
#define SYS_AUDIO     42  // op=a1: play/query the audio output
#define SYS_SPAWNWIN  43  // run a program in a NEW window; a1 = winspawn *
#define SYS_DRAG      44  // drag and drop; a1 = op
#define SYS_SETTING   45  // op=a1, key=a2, value=a3 -> the value, or -1
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

#define SYS_SIGNAL    48  // op=a1; the ops are SIGOP_* in kernel/kernel/signal.h

// SYS_SETTING operations.
#define SETOP_GET        0   // a2 = SET_*                     -> value
#define SETOP_SET        1   // a2 = SET_*, a3 = value          -> the value set
#define SETOP_THEME_NAME 2   // a2 = theme index, a3 = char *buf (>= 32 bytes)
#define SETOP_PALETTE    3   // a2 = struct ui_palette *, a3 = its size
// The colours a program should draw with, from the current theme.
struct ui_palette {
    unsigned int win, panel, alt, text, dim, line, edge;
    unsigned int accent, accent2, sel, hot, btn, btndn;
    unsigned int bar, bar2, warn, good;
};


// The settings themselves.
#define SET_THEME        0   // 0 .. SET_THEME_COUNT-1
#define SET_THEME_COUNT  1   // read-only
#define SET_KEY_DELAY    2   // auto-repeat delay, ms
#define SET_KEY_RATE     3   // auto-repeat interval, ms
#define SET_MOUSE_SPEED  4   // pointer speed, percent
#define SET_DBLCLICK     5   // double-click window, ms


#define DRAG_BEGIN  0     // a2 = payload, a3 = label shown under the cursor
#define DRAG_TAKE   1     // a2 = buffer, a3 = size; -> bytes, or -1
#define DRAG_CANCEL 2

#define AU_INFO   0       // -> 1 if a real codec is present, else 0
#define AU_WRITE  1       // a2 = int16 stereo frames, a3 = frame count
#define AU_QUEUED 2       // -> frames still waiting to be played
#define AU_STOP   3
#define AU_VOLUME 4       // a2 = 0..100, or -1 to just read it back

#define PC_KILL   0
#define PC_STOP   1
#define PC_CONT   2

#define FONT_INFO  0      // -> (cell_w << 16) | cell_h, or -1 if no font
#define FONT_ATLAS 1      // copy 128 glyph cells to a2 (len a3); -> bytes

// SYS_POLLMOUSE output. Coordinates are relative to the caller's own drawing
// surface, so a program never learns where its window sits on screen.
struct umouse {
    short x, y;
    unsigned char buttons, pressed, released;
    signed char wheel;
    unsigned char drag;      // 0 none, 1 dragging over, 2 dropped here
};

// SYS_MSGBOX request (a1 points at this). `kind` is one of the MB_* values in
// wm.h. Every string may be NULL.
struct msgbox_req {
    int         kind;
    const char *code;
    const char *title;
    const char *text;
    const char *detail;
};

// SYS_SPAWNWIN request (a1 points at this). `arg` may be NULL.
struct winspawn {
    const char *path;
    const char *arg;
};

// SYS_TIME output: broken-down UTC time from the CMOS RTC.
struct sys_tm { int sec, min, hour, day, mon, year; };

// SYS_USBFS: one directory entry returned to userland.
struct usb_ent { char name[16]; unsigned int size; int is_dir; };

// SYS_USBFS request/response (a1 points at this).
struct usbfs_req {
    int op;              // USBFS_*
    const char *path;    // fs path on the stick ("/" = root)
    void *buf;           // LIST: struct usb_ent[len]; READ/WRITE: raw bytes
    unsigned int len;    // LIST: max entries; READ/WRITE: buffer length
    int result;          // OUT: entry count (LIST) or bytes (READ/WRITE), -1 err
};
#define USBFS_MOUNT 0    // auto-mount the FAT32 data stick; result = 1/0
#define USBFS_LIST  1
#define USBFS_READ  2
#define USBFS_WRITE 3

// SYS_NET request/response (a1 points at this).
struct net_req {
    int op;              // NET_*
    const char *a;       // host / dotted-quad (PING, RESOLVE, HTTPGET)
    const char *b;       // path (HTTPGET)
    void *buf;           // OUT: CONFIG -> struct net_info; RESOLVE -> uint32 ip;
                         //      HTTPGET -> response body
    unsigned int len;    // buf capacity
    unsigned int arg;    // HTTPGET: TCP port
    const char *hdr;     // extra request headers, CRLF-terminated, or 0
    unsigned int hdrlen;
    int result;          // OUT: op-dependent (see below), -1 on error
};
#define NET_UP      0    // bring the link up (DHCP); result = 1/0
#define NET_CONFIG  1    // buf <- struct net_info; result = 0/-1 (down)
#define NET_PING    2    // ping a (ip string); result = rtt_ms, or -1
#define NET_RESOLVE 3    // resolve a (host) -> buf (uint32 host-order ip); 0/-1
#define NET_HTTPGET 4    // GET a=host b=path arg=port -> buf(body); result=bytes
#define NET_STATUS  5    // buf <- struct net_status; result = 1 if a card exists
#define NET_LOG     6    // buf <- struct net_log_ent[len]; result = how many
                         // arg != 0 clears the log instead
/* A fetch that does not hold the machine still while it runs: START it, POLL
 * it until it stops saying PENDING, then TAKE the bytes. Each POLL runs the
 * transfer for a few milliseconds and returns, so everything else on the
 * machine keeps going in between. */
#define NET_FSTART  7    // a=host b=path arg=port|flags; result = 1 accepted
#define NET_FPOLL   8    // result = -2 pending, else bytes or -1
                         // arg (OUT) = bytes arrived so far
#define NET_FTAKE   9    // buf <- the body; result = bytes
#define NET_FSLOTS  10   // result = how many fetches may run at once
#define NET_FCANCEL 11   // arg = slot; give it up
/* Set in `arg` alongside the port to fetch over TLS. A flag rather than a
 * separate op, because everything else about the request is identical. */
#define NET_HTTPGET_TLS 0x10000
/* Return the whole response -- status line, headers, blank line, body --
 * instead of the body alone. What a browser needs to follow a redirect. */
#define NET_HTTPGET_RAW 0x20000
#define NET_HTTPGET_POST 0x20000000u  // there is a body: POST it



struct net_info { unsigned int ip, gw, mask, dns; };  // all host byte order

// What SYS_NET's NET_STATUS op reports. Unlike NET_CONFIG this answers even
// when the link is down -- a settings page needs to say "there is a card and
// it has no lease" as clearly as it says "here is your address".
struct net_status {
    unsigned int ip, gw, mask, dns;   // host byte order; zero unless `up`
    unsigned char mac[6];
    unsigned char link;               // 1 = carrier detected
    unsigned char up;                 // 1 = holding a DHCP lease
    char driver[16];                  // "e1000", "rtl8125", ... "" if no card
};


// SYS_GFX: a1 = op, remaining args op-dependent.
//   GFX_BLIT: a2 = ARGB pixel buffer, a3 = (w << 16) | h. Copies the frame into
//             the pane and composites+presents it. -> 0/-1.
//   GFX_INFO: -> (interior_w << 16) | interior_h, the pane's pixel size.
//   GFX_END:  leave graphics mode, restore the text grid. -> 0.
#define GFX_BLIT 0
#define GFX_INFO 1
#define GFX_END  2

// SYS_POWER ops.
#define POWER_REBOOT 0
#define POWER_OFF    1
#define POWER_SLEEP  2

// SYS_SYSINFO selectors.
#define SI_MEM    0   // one struct si_mem
#define SI_PROCS  1   // an array of struct si_proc; returns bytes written
#define SI_UNAME  2   // a NUL-terminated string
#define SI_DEVICES 3  // an array of struct si_dev; returns bytes written

struct si_mem {
    uint64_t page_size;
    uint64_t total_frames;
    uint64_t free_frames;
};


// --- the device list (SI_DEVICES) ---------------------------------------
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

#define SI_NAME_MAX 32
struct si_proc {
    int pid;
    int parent_pid;
    int state;          // proc_state_t
    int pane;           // 1 if this process owns a pane
    int stopped;        // 1 if suspended by the task manager
    unsigned long long ticks;   // timer ticks spent on the CPU
    char name[SI_NAME_MAX];
};

// File redirection is expressed as PATHS (the kernel opens them for the child);
// pipe endpoints are expressed as pipe INDICES from SYS_PIPE. A stage in a
// pipeline gets its stdin/stdout from pipes; the ends of the pipeline may also
// have a file redirect. A pipe wins over a path if both are somehow set.
struct spawn_req {
    const char *path;
    const char *const *argv;
    int argc;
    const char *in_path;    // NULL: keyboard (unless in_pipe is set)
    const char *out_path;   // NULL: pane (unless out_pipe is set)
    int append;             // open out_path for append rather than truncate
    int in_pipe;            // pipe index for stdin, or -1
    int out_pipe;           // pipe index for stdout, or -1
};

void syscall_init(void);
void enter_usermode(uint64_t entry, uint64_t user_stack);

#endif
