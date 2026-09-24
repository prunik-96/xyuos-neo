#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include <sys/types.h>    // ssize_t, off_t

#ifdef __cplusplus
extern "C" {
#endif

// Raw console write (no descriptor). Prefer write(1, ...) in new code.
long xyuos_console_write(const void *buf, unsigned long len);

// POSIX descriptor layer. Real files are offset by 3 so that 0/1/2 keep their
// conventional meaning (stdin/stdout/stderr) -- see libc/src/posix.c.
/* What access() asks about. There is no access() here yet, but portable
 * code tests these against its own flags whether or not it calls it. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

long read(int fd, void *buf, unsigned long len);
long write(int fd, const void *buf, unsigned long len);
int  close(int fd);
long lseek(int fd, long offset, int whence);
int  unlink(const char *path);
int  ftruncate(int fd, unsigned long length);
/* The three descriptors every program starts with. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

char *getcwd(char *buf, unsigned long size);
int   chdir(const char *path);
int   fsync(int fd);
int   fdatasync(int fd);
int  execvp(const char *file, char *const argv[]);

// Empty (this OS has no environment), but present and NULL-terminated because
// hosted code walks it.
extern char **environ;

long xyuos_open(const char *path);
long xyuos_read(long fd, void *buf, unsigned long len);
long xyuos_writefd(long fd, const void *buf, unsigned long len);
void xyuos_close(long fd);
long xyuos_listdir(const char *path, char *buf, unsigned long len);
char xyuos_readkey(void);
void xyuos_hyper(void);
void _exit(int code) __attribute__((noreturn));

// Filesystem mutations (absolute paths). Return 0 on success, -1 on error.
long xyuos_create(const char *path);
long xyuos_mkdir(const char *path);
long xyuos_unlink(const char *path);
long xyuos_rename(const char *oldpath, const char *newpath);

// --- random access + metadata ---------------------------------------------
// Values match POSIX so <stdio.h> can pass them straight through.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// Layout MUST match `struct vfs_stat` in kernel/fs/vfs.h -- the kernel writes
// this struct directly into the caller's buffer.
struct xyuos_stat {
    unsigned int size;
    unsigned int inode;
    int          is_dir;
    unsigned int mtime;   // last-modified, seconds since the Unix epoch (UTC)
};

// Grow the heap by `increment` bytes and return the PREVIOUS break, or
// (void *)-1 if the request cannot be satisfied. Memory comes back zeroed.
void *sbrk(long increment);

/* Process control. spawn() returns immediately with the child's pid; there is
 * no fork() -- the kernel builds the child's address space from an ELF, so
 * there is nothing to copy. waitpid(-1) waits for any child. */
int spawn(const char *path, char *const argv[], int argc);
int waitpid(int pid);

/* spawn() with the child's stdin/stdout redirected to files. NULL leaves a
 * stream on the terminal. `append` opens out_path for append instead of
 * truncating it. */
int spawn_redir(const char *path, char *const argv[], int argc,
                const char *in_path, const char *out_path, int append);

/* spawn() with file redirection and/or pipe endpoints. A pipe index (from
 * pipe_new()) wins over a path for the same stream; -1 leaves it alone. */
int spawn_full(const char *path, char *const argv[], int argc,
               const char *in_path, const char *out_path, int append,
               int in_pipe, int out_pipe);

/* An in-kernel pipe: one writer, one reader, blocking. pipe_new() returns an
 * index to hand to spawn_full(); pipe_close() tears a half-wired one down. */
int  pipe_new(void);
void pipe_close(int idx);

long xyuos_read_stdin(void *buf, unsigned long len);

/* System information: SI_MEM, SI_PROCS or SI_UNAME (see xyuos_syscall.h).
 * Returns the number of bytes written, or -1. */
long xyuos_sysinfo(int what, void *buf, unsigned long len);

unsigned int sleep(unsigned int seconds);
unsigned int sleep_ms(unsigned int ms);

/* Interrupt a process by pid (there is one signal: terminate). Returns 0, or
 * -1 if there is no such process. */
int kill(int pid);

/* Suspend / resume a process by pid. 0 on success, -1 on error. A suspended
 * process keeps its memory and its windows; it simply stops being picked. */
int proc_stop(int pid);
int proc_cont(int pid);

/* --- desktop message boxes ---
 * Put a modal dialog on the screen, the way the system itself reports a
 * program that crashed. For failures a person needs to see even when they are
 * not looking at the window that failed. */
#define MSG_ERROR 0
#define MSG_WARN  1
#define MSG_INFO  2
void message_box(int kind, const char *code, const char *title,
                 const char *text, const char *detail);

/* --- audio output ---
 * 48 kHz, 16-bit signed, stereo -- the one format the hardware guarantees.
 * audio_play() queues frames and returns how many were accepted; it does not
 * block, so a player tops the queue up as it drains. */
int  audio_present(void);
int  audio_play(const short *frames, int nframes);
int  audio_pending(void);
void audio_flush(void);
int  audio_volume(int pct);          /* pct < 0 just reads it back */

/* Run a program in a NEW desktop window, with an optional single argument.
 * Returns the child's pid, or -1.
 *
 * Different from spawn(): a spawned child shares the pane it was started
 * from, which is what a shell pipeline wants and the opposite of what opening
 * a document wants. */
int spawn_window(const char *path, const char *arg);

/* --- drag and drop ---
 * drag_begin() hands the compositor a payload (for a file manager, a path)
 * and a short label to show under the cursor, and from then on the
 * compositor owns the drag -- the pointer is about to leave this window.
 * Windows the pointer passes over receive events with drag == DRAG_OVER; the
 * one it is released on receives DRAG_DROP and collects the payload with
 * drag_take(). Returns 0 on success. */
int  drag_begin(const char *payload, const char *label);
int  drag_take(char *buf, int max);      /* bytes, or -1 if nothing waiting */
void drag_cancel(void);

/* Power control. op: 0 = reboot, 1 = power off, 2 = sleep (halt until a key).
 * Reboot/off do not return; sleep returns when a key wakes the machine. */
void xyuos_power(int op);
#define XYUOS_REBOOT 0
#define XYUOS_OFF    1
#define XYUOS_SLEEP  2

/* Wall-clock time from the CMOS RTC (UTC on most machines). */
struct xyuos_tm { int sec, min, hour, day, mon, year; };
void xyuos_time(struct xyuos_tm *out);

/* USB FAT32 data stick. usb_mount() auto-mounts the first FAT32 stick (returns
 * 1 if found). usb_list() fills up to `max` entries of a directory, returns the
 * count. usb_readfile() reads a whole file into buf, returns bytes. All -1 on
 * error. */
struct usb_ent { char name[16]; unsigned int size; int is_dir; };
int usb_mount(void);
int usb_list(const char *path, struct usb_ent *out, int max);
int usb_readfile(const char *path, void *buf, int max);
/* usb_writefile() creates/overwrites a file on the stick (8.3 names). Returns
 * bytes written, or -1. */
int usb_writefile(const char *path, const void *buf, int len);

/* Multi-core parallel benchmark: counts primes below n on one core, then on
 * every core, timing each. Layout must match struct smp_result in the kernel's
 * syscall.h. Returns 0. */
struct smp_result {
    int          cores;
    unsigned int ms_single;
    unsigned int ms_multi;
    unsigned long long primes;
};
int smp_bench(unsigned long long n, struct smp_result *out);

/* --- networking ---
 * The stack leases an address over DHCP on first use. All addresses are host
 * byte order. net_up() forces the link up (returns 1 on success); the other
 * calls bring it up on demand. */
struct net_info { unsigned int ip, gw, mask, dns; };

/* Everything net_config() reports, plus the things it cannot: which driver
 * bound, its hardware address, and whether there is a carrier. Answered even
 * when the link is down, which is exactly when somebody is asking. */
struct net_status {
    unsigned int ip, gw, mask, dns;   /* zero unless `up` */
    unsigned char mac[6];
    unsigned char link;               /* 1 = carrier detected      */
    unsigned char up;                 /* 1 = holding a DHCP lease  */
    char driver[16];                  /* "" when no card was found */
};
int net_up(void);
int net_config(struct net_info *out);          /* 0 on success, -1 if down */
int net_status(struct net_status *out);        /* 1 if a card exists, else 0 */
int net_ping(const char *host);                /* RTT in MICROseconds, or -1 */
int net_resolve(const char *host, unsigned int *ip);   /* 0 / -1 */
/* HTTP GET; writes up to `max` body bytes into buf, returns count or -1. */
/* --- the request log -----------------------------------------------------
 *
 * One entry per fetch the system performed: where it went, how long it took,
 * whether a handshake was needed. Mirrors struct net_log_ent in
 * kernel/net/net.h -- keep in step. */
#define NET_LOG_TLS     1        /* https rather than http                  */
#define NET_LOG_REUSED  2        /* no handshake: a connection was already open */
#define NET_LOG_FAIL    4        /* it did not come back                    */
#define NET_LOG_CACHED  8        /* never reached the network at all        */

struct net_log_ent {
    unsigned int   start_ms;
    unsigned int   dur_ms;
    int            bytes;
    short          status;
    unsigned char  flags;
    unsigned char  pad;
    char           host[64];
    char           path[144];
};

/* Fills `out` with up to `max` entries, oldest first, and returns how many. */
int net_log(struct net_log_ent *out, int max);
void net_log_reset(void);

int net_http_get(const char *host, const char *path, int port,
                 void *buf, int max);
/* The same over TLS. The connection is encrypted, but the server's identity is
 * NOT yet checked -- see kernel/net/tls.c. */
int net_https_get(const char *host, const char *path, int port,
                  void *buf, int max);
/* The whole response, headers and all, so the caller can read the status code
 * and the content type. `tls` picks https. Returns bytes, or -1. */
/* --- fetching without freezing -------------------------------------------
 *
 * net_fetch() does not return until the transfer is over, and while it is
 * inside the kernel nothing else on the machine runs. These three do the same
 * work in slices: begin it, ask after it until it stops saying PENDING, then
 * take the bytes. The caller keeps its window alive in between. */
#define NET_FETCH_PENDING (-2)

int net_fetch_begin(const char *host, const char *path, int port, int tls,
                    int keep_headers, const char *body, int blen,
                    const char *hdr);
/* net_fetch_begin returns the SLOT the fetch was given, or -1 if every slot
 * is busy; the other two name that slot. Four may be in flight at once, each
 * on its own connection -- which is what lets a page load its pictures
 * alongside one another instead of one after the next. */
/* PENDING while it runs, else the byte count or -1. *progress, if given, is
 * how much has arrived so far. */
int net_fetch_check(int slot, int *progress);
int net_fetch_done(int slot, void *buf, int max);
/* How many may be in flight. Asked rather than assumed, so there is one
 * number and not two that drift apart. */
int net_fetch_slots(void);
/* Give up on a fetch. Every slot a program holds must be cancelled before it
 * exits -- the kernel cannot clean up after a program that simply leaves. */
void net_fetch_cancel(int slot);

int net_fetch(const char *host, const char *path, int port, int tls,
              void *buf, int max);

// New absolute offset, or -1 on error.
long xyuos_seek(long fd, long offset, int whence);
// 0 on success, -1 if the path does not exist.
long xyuos_stat(const char *path, struct xyuos_stat *out);
// Resize an open file. 0 on success, -1 on error.
long xyuos_ftruncate(long fd, unsigned long length);

// Key events (arrows + modifiers) for TUI programs (editor, file manager).
#define XKEY_CHAR   0
#define XKEY_UP     1
#define XKEY_DOWN   2
#define XKEY_LEFT   3
#define XKEY_RIGHT  4
#define XKEY_ENTER  5
#define XKEY_BKSP   6
#define XKEY_RESIZE 7   // the window changed size: redraw
#define XKEY_PGUP   8
#define XKEY_PGDN   9
#define XKEY_DEL    10
#define XKEY_HOME   11
#define XKEY_END    12
#define XKEY_ESC    13
#define XKEY_F1     0x10  // .. XKEY_F12 == 0x1B
#define XKEY_F(n)   (0x10 + (n) - 1)

// The modifier keys as keys in their own right. Only delivered after
// key_events_raw(1) -- see below.
#define XKEY_SHIFT  0x0E
#define XKEY_CTRL   0x0F
#define XKEY_ALT    0x1C
#define XKEY_SUPER  0x1D
// The window lost focus: treat every key you think is held as released.
#define XKEY_FOCUSOUT 0x1E

#define XMOD_SHIFT  0x01
#define XMOD_CTRL   0x02
#define XMOD_ALT    0x04
#define XMOD_SUPER  0x08

typedef struct {
    unsigned char code;    // XKEY_*
    char          ascii;   // valid when code == XKEY_CHAR
    unsigned char mods;    // XMOD_* bitmask
    unsigned char pressed; // 1 = key went down, 0 = came up (see key_events_raw)
} key_event_t;

// Ask for the WHOLE keyboard, not just what was typed: key-up events, and the
// modifier keys themselves as XKEY_SHIFT/CTRL/ALT/SUPER.
//
// Off by default, deliberately. A program that only wants text should never
// see a release -- it would act on every key twice. A program that needs to
// know what is being HELD -- a game, anything with a "walk while this is
// down" -- turns it on and tracks the state itself.
void key_events_raw(int on);

key_event_t xyuos_read_event(void);

// --- raw graphics surface for the caller's pane (SYS_GFX) ---
// gfx_blit() copies a w*h ARGB (0x00RRGGBB) frame into the pane, scaled to fit,
// and presents it. gfx_info() reports the pane's interior pixel size.
// gfx_end() leaves graphics mode and restores the text grid.
int  gfx_blit(const void *argb, int w, int h);
void gfx_info(int *w, int *h);
void gfx_end(void);

// Non-blocking key event: 1 and fills *ev if one was queued, else 0.
int  poll_event(key_event_t *ev);

// --- pointer (SYS_POLLMOUSE) ---
#define MB_LEFT   0x01
#define MB_RIGHT  0x02
#define MB_MIDDLE 0x04

/* mouse_event_t.drag */
#define DRAG_NONE 0
#define DRAG_OVER 1             /* something is being dragged over the window */
#define DRAG_DROP 2             /* and it was let go here */
#define DRAG_LEAVE 3            /* it moved off this window again */

typedef struct {
    short x, y;                 // relative to the caller's own surface
    unsigned char buttons;      // MB_* held right now
    unsigned char pressed;      // went down in this event
    unsigned char released;     // came up in this event
    signed char wheel;          // notches, positive = away from the user
    unsigned char drag;         // DRAG_NONE / DRAG_OVER / DRAG_DROP
} mouse_event_t;

// Non-blocking pointer event: 1 and fills *ev if one was queued, else 0.
int  poll_mouse(mouse_event_t *ev);

// --- the window manager's anti-aliased font (SYS_FONT) ---
// font_cell() reports the monospace cell size; font_atlas() copies the
// 128 glyph coverage bitmaps (128 * cell_w * cell_h bytes) into `buf`.
// A GUI program fetches the atlas once and renders text from it.
int  font_cell(int *w, int *h);
long font_atlas(void *buf, unsigned long len);

// Milliseconds since boot (a monotonic clock for game/animation loops).
unsigned int uptime_ms(void);

// Console dimensions in character cells.
void xyuos_termsize(unsigned int *cols, unsigned int *rows);

#ifdef __cplusplus
}
#endif

#endif
