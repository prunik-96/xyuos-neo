#include <unistd.h>
#include <string.h>
#include <tty.h>
#include "xyuos_syscall.h"

long xyuos_console_write(const void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_WRITE, (long)buf, (long)len, 0);
}

long xyuos_open(const char *path) {
    return xyuos_syscall3(SYS_OPEN, (long)path, 0, 0);
}

long xyuos_read(long fd, void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_READ, fd, (long)buf, (long)len);
}

void xyuos_close(long fd) {
    xyuos_syscall3(SYS_CLOSE, fd, 0, 0);
}

long xyuos_listdir(const char *path, char *buf, unsigned long len) {
    return xyuos_syscall3(SYS_LISTDIR, (long)path, (long)buf, (long)len);
}

long xyuos_writefd(long fd, const void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_WRITEFILE, fd, (long)buf, (long)len);
}

char xyuos_readkey(void) {
    return (char)xyuos_syscall3(SYS_READKEY, 0, 0, 0);
}

void xyuos_hyper(void) {
    xyuos_syscall3(SYS_HYPER, 0, 0, 0);
}

void *sbrk(long increment) {
    long r = xyuos_syscall3(SYS_SBRK, increment, 0, 0);
    return (r == -1) ? (void *)-1 : (void *)r;
}

/* Start `path` as a separate process and return its pid without waiting. argv
 * is the usual NULL-terminatable array; argc is passed explicitly because the
 * kernel copies exactly that many strings out of user space. */
int spawn(const char *path, char *const argv[], int argc) {
    return (int)xyuos_syscall3(SYS_SPAWN, (long)path, (long)argv, (long)argc);
}

/* Block until a child exits and return its exit code. pid < 0 waits for any
 * child. Returns -1 if there is no such child. */
int waitpid(int pid) {
    return (int)xyuos_syscall3(SYS_WAIT, (long)pid, 0, 0);
}

/* Like spawn(), but the child's stdin/stdout come from files. Passing paths
 * rather than descriptors is deliberate: the kernel opens them for the child,
 * so there is no fd table to inherit. */
int spawn_redir(const char *path, char *const argv[], int argc,
                const char *in_path, const char *out_path, int append) {
    struct spawn_req req;
    req.path = path;
    req.argv = (const char *const *)argv;
    req.argc = argc;
    req.in_path = in_path;
    req.out_path = out_path;
    req.append = append;
    req.in_pipe = -1;
    req.out_pipe = -1;
    return (int)xyuos_syscall3(SYS_SPAWN2, (long)&req, 0, 0);
}

/* Full spawn: file redirection AND/OR pipe endpoints. A pipe index (from
 * pipe()) wins over a path for the same stream. -1 leaves that stream alone. */
int spawn_full(const char *path, char *const argv[], int argc,
               const char *in_path, const char *out_path, int append,
               int in_pipe, int out_pipe) {
    struct spawn_req req;
    req.path = path;
    req.argv = (const char *const *)argv;
    req.argc = argc;
    req.in_path = in_path;
    req.out_path = out_path;
    req.append = append;
    req.in_pipe = in_pipe;
    req.out_pipe = out_pipe;
    return (int)xyuos_syscall3(SYS_SPAWN2, (long)&req, 0, 0);
}

/* Create a pipe; returns its index, or -1. Pass the index as a stage's
 * in_pipe/out_pipe to spawn_full(). */
int pipe_new(void)          { return (int)xyuos_syscall3(SYS_PIPE, 0, 0, 0); }
void pipe_close(int idx)    { xyuos_syscall3(SYS_PIPECLOSE, (long)idx, 0, 0); }

long xyuos_read_stdin(void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_READSTD, (long)buf, (long)len, 0);
}

long xyuos_sysinfo(int what, void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_SYSINFO, (long)what, (long)buf, (long)len);
}

unsigned int sleep_ms(unsigned int ms) {
    xyuos_syscall3(SYS_SLEEP, (long)ms, 0, 0);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    return sleep_ms(seconds * 1000);
}

void xyuos_power(int op) {
    xyuos_syscall3(SYS_POWER, (long)op, 0, 0);
}

void xyuos_time(struct xyuos_tm *out) {
    xyuos_syscall3(SYS_TIME, (long)out, 0, 0);
}

/* Must match struct usbfs_req in the kernel's syscall.h. */
struct usbfs_req { int op; const char *path; void *buf; unsigned int len; int result; };

int usb_mount(void) {
    struct usbfs_req rq = { 0 /*MOUNT*/, 0, 0, 0, -1 };
    xyuos_syscall3(SYS_USBFS, (long)&rq, 0, 0);
    return rq.result;
}

int usb_list(const char *path, struct usb_ent *out, int max) {
    struct usbfs_req rq = { 1 /*LIST*/, path, out, (unsigned)max, -1 };
    xyuos_syscall3(SYS_USBFS, (long)&rq, 0, 0);
    return rq.result;
}

int usb_readfile(const char *path, void *buf, int max) {
    struct usbfs_req rq = { 2 /*READ*/, path, buf, (unsigned)max, -1 };
    xyuos_syscall3(SYS_USBFS, (long)&rq, 0, 0);
    return rq.result;
}

int usb_writefile(const char *path, const void *buf, int len) {
    struct usbfs_req rq = { 3 /*WRITE*/, path, (void *)buf, (unsigned)len, -1 };
    xyuos_syscall3(SYS_USBFS, (long)&rq, 0, 0);
    return rq.result;
}

int smp_bench(unsigned long long n, struct smp_result *out) {
    return (int)xyuos_syscall3(SYS_SMP, (long)n, (long)out, 0);
}

/* --- raw graphics surface (SYS_GFX) --- */
int gfx_blit(const void *argb, int w, int h) {
    long dim = ((long)(w & 0xFFFF) << 16) | (long)(h & 0xFFFF);
    return (int)xyuos_syscall3(SYS_GFX, GFX_BLIT, (long)argb, dim);
}
void gfx_info(int *w, int *h) {
    long r = xyuos_syscall3(SYS_GFX, GFX_INFO, 0, 0);
    /* -1 means "you have no window". Masking that would report 65535x65535
     * and send the caller off to allocate seventeen gigabytes. */
    if (r < 0) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = (int)((r >> 16) & 0xFFFF);
    if (h) *h = (int)(r & 0xFFFF);
}
void gfx_end(void) { xyuos_syscall3(SYS_GFX, GFX_END, 0, 0); }

/* Non-blocking key event. Returns 1 and fills *ev if one was waiting, else 0. */
int poll_event(key_event_t *ev) {
    long packed = xyuos_syscall3(SYS_POLLEVENT, POLLEV_NEXT, 0, 0);
    if (packed == -1) return 0;
    if (ev) {
        ev->ascii   = (char)(packed & 0xFF);
        ev->code    = (unsigned char)((packed >> 8) & 0xFF);
        ev->mods    = (unsigned char)((packed >> 16) & 0xFF);
        ev->pressed = (unsigned char)((packed >> 24) & 1);
    }
    return 1;
}

void key_events_raw(int on) {
    xyuos_syscall3(SYS_POLLEVENT, POLLEV_KEYUP, on ? 1 : 0, 0);
}

int poll_mouse(mouse_event_t *ev) {
    struct umouse_u {
        short x, y;
        unsigned char b, p, r;
        signed char w;
        unsigned char drag;
    } u;
    if (xyuos_syscall3(SYS_POLLMOUSE, (long)&u, 0, 0) != 0) return 0;
    if (ev) {
        ev->x = u.x; ev->y = u.y;
        ev->buttons = u.b; ev->pressed = u.p; ev->released = u.r;
        ev->wheel = u.w;
        ev->drag = u.drag;
    }
    return 1;
}

int drag_begin(const char *payload, const char *label) {
    return (int)xyuos_syscall3(SYS_DRAG, DRAG_BEGIN, (long)payload, (long)label);
}
int drag_take(char *buf, int max) {
    return (int)xyuos_syscall3(SYS_DRAG, DRAG_TAKE, (long)buf, max);
}
void drag_cancel(void) { xyuos_syscall3(SYS_DRAG, DRAG_CANCEL, 0, 0); }

int spawn_window(const char *path, const char *arg) {
    struct winspawn rq = { path, arg };
    return (int)xyuos_syscall3(SYS_SPAWNWIN, (long)&rq, 0, 0);
}

void message_box(int kind, const char *code, const char *title,
                 const char *text, const char *detail) {
    struct msgbox_req rq = { kind, code, title, text, detail };
    xyuos_syscall3(SYS_MSGBOX, (long)&rq, 0, 0);
}

int  audio_present(void) { return (int)xyuos_syscall3(SYS_AUDIO, AU_INFO, 0, 0); }
int  audio_pending(void) { return (int)xyuos_syscall3(SYS_AUDIO, AU_QUEUED, 0, 0); }
void audio_flush(void)   { xyuos_syscall3(SYS_AUDIO, AU_STOP, 0, 0); }
int  audio_volume(int pct) { return (int)xyuos_syscall3(SYS_AUDIO, AU_VOLUME, pct, 0); }

int audio_play(const short *frames, int nframes) {
    return (int)xyuos_syscall3(SYS_AUDIO, AU_WRITE, (long)frames, nframes);
}

int proc_stop(int pid) { return (int)xyuos_syscall3(SYS_PROCCTL, PC_STOP, pid, 0); }
int proc_cont(int pid) { return (int)xyuos_syscall3(SYS_PROCCTL, PC_CONT, pid, 0); }

int font_cell(int *w, int *h) {
    long r = xyuos_syscall3(SYS_FONT, FONT_INFO, 0, 0);
    if (r < 0) return 0;
    if (w) *w = (int)((r >> 16) & 0xFFFF);
    if (h) *h = (int)(r & 0xFFFF);
    return 1;
}

long font_atlas(void *buf, unsigned long len) {
    return xyuos_syscall3(SYS_FONT, FONT_ATLAS, (long)buf, (long)len);
}

unsigned int uptime_ms(void) { return (unsigned int)xyuos_syscall3(SYS_UPTIME, 0, 0, 0); }

/* --- networking (see unistd.h) --- */

int net_up(void) {
    struct net_req rq = { NET_UP, 0, 0, 0, 0, 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_config(struct net_info *out) {
    struct net_req rq = { NET_CONFIG, 0, 0, out, sizeof(*out), 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_status(struct net_status *out) {
    struct net_req rq = { NET_STATUS, 0, 0, out, sizeof(*out), 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result > 0 ? rq.result : 0;
}

int net_ping(const char *host) {
    struct net_req rq = { NET_PING, host, 0, 0, 0, 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_resolve(const char *host, unsigned int *ip) {
    struct net_req rq = { NET_RESOLVE, host, 0, ip, 4, 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_http_get(const char *host, const char *path, int port,
                 void *buf, int max) {
    struct net_req rq = { NET_HTTPGET, host, path, buf, (unsigned)max,
                          (unsigned)port, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_https_get(const char *host, const char *path, int port,
                  void *buf, int max) {
    struct net_req rq = { NET_HTTPGET, host, path, buf, (unsigned)max,
                          (unsigned)port | NET_HTTPGET_TLS, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_fetch_begin(const char *host, const char *path, int port, int tls,
                    int keep_headers, const char *body, int blen,
                    const char *hdr) {
    unsigned arg = (unsigned)port;
    if (tls) arg |= NET_HTTPGET_TLS;
    if (keep_headers) arg |= NET_HTTPGET_RAW;
    if (body && blen > 0) arg |= NET_HTTPGET_POST;
    else { body = 0; blen = 0; }
    /* The body travels in the two fields a GET left empty. */
    int hl = hdr ? (int)strlen(hdr) : 0;
    struct net_req rq = { NET_FSTART, host, path, (void *)body, (unsigned)blen,
                          arg, hl ? hdr : 0, (unsigned)hl, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

void net_fetch_cancel(int slot) {
    struct net_req rq = { .op = NET_FCANCEL, .arg = (unsigned)slot,
                          .result = -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
}

int net_fetch_slots(void) {
    struct net_req rq = { .op = NET_FSLOTS, .result = -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result > 0 ? rq.result : 1;
}

int net_fetch_check(int slot, int *progress) {
    /* The slot travels in `len` and the progress comes back in `arg`: a poll
     * uses neither for anything else. */
    struct net_req rq = { .op = NET_FPOLL, .len = (unsigned)slot,
                          .result = -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    if (progress) *progress = (int)rq.arg;
    return rq.result;
}

int net_fetch_done(int slot, void *buf, int max) {
    struct net_req rq = { .op = NET_FTAKE, .buf = buf, .len = (unsigned)max,
                          .arg = (unsigned)slot, .result = -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

int net_log(struct net_log_ent *out, int max) {
    struct net_req rq = { NET_LOG, 0, 0, out, (unsigned)max, 0, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result < 0 ? 0 : rq.result;
}

void net_log_reset(void) {
    struct net_req rq = { NET_LOG, 0, 0, 0, 0, 1, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
}

int net_fetch(const char *host, const char *path, int port, int tls,
              void *buf, int max) {
    unsigned arg = (unsigned)port | NET_HTTPGET_RAW;
    if (tls) arg |= NET_HTTPGET_TLS;
    struct net_req rq = { NET_HTTPGET, host, path, buf, (unsigned)max, arg, -1 };
    xyuos_syscall3(SYS_NET, (long)&rq, 0, 0);
    return rq.result;
}

/* --- pane (tty) control ---
 * The grid itself lives in the kernel; these reach the caller's own pane and
 * no other. Ordinary write()/printf() output goes to the same pane. */

void tty_clear(void)      { xyuos_syscall3(SYS_TTYCTL, TTY_CLEAR, 0, 0); }
void tty_reset_color(void){ xyuos_syscall3(SYS_TTYCTL, TTY_RESETCOLOR, 0, 0); }
void tty_erase_line(void) { xyuos_syscall3(SYS_TTYCTL, TTY_ERASELINE, 0, 0); }

void tty_move(int row, int col) {
    xyuos_syscall3(SYS_TTYCTL, TTY_MOVE, row, col);
}

void tty_set_color(int fg, int bg) {
    xyuos_syscall3(SYS_TTYCTL, TTY_SETCOLOR, fg, bg);
}

void tty_getcur(int *row, int *col) {
    long r = xyuos_syscall3(SYS_TTYCTL, TTY_GETCUR, 0, 0);
    if (r < 0) { *row = 0; *col = 0; return; }
    *row = (int)((r >> 16) & 0xFFFF);
    *col = (int)(r & 0xFFFF);
}

void tty_size(int *cols, int *rows) {
    long r = xyuos_syscall3(SYS_TTYCTL, TTY_SIZE, 0, 0);
    if (r < 0) { *cols = 80; *rows = 25; return; }
    *cols = (int)((r >> 16) & 0xFFFF);
    *rows = (int)(r & 0xFFFF);
}

long xyuos_seek(long fd, long offset, int whence) {
    return xyuos_syscall3(SYS_SEEK, fd, offset, (long)whence);
}

long xyuos_stat(const char *path, struct xyuos_stat *out) {
    return xyuos_syscall3(SYS_STAT, (long)path, (long)out, 0);
}

long xyuos_ftruncate(long fd, unsigned long length) {
    return xyuos_syscall3(SYS_TRUNCATE, fd, (long)length, 0);
}

long xyuos_create(const char *path) {
    return xyuos_syscall3(SYS_CREATE, (long)path, 0, 0);
}

long xyuos_mkdir(const char *path) {
    return xyuos_syscall3(SYS_MKDIR, (long)path, 0, 0);
}

/* POSIX mkdir(2). `mode` is ignored -- this OS has no permission bits. */
int mkdir(const char *path, unsigned int mode) {
    (void)mode;
    return (int)xyuos_syscall3(SYS_MKDIR, (long)path, 0, 0);
}

long xyuos_unlink(const char *path) {
    return xyuos_syscall3(SYS_UNLINK, (long)path, 0, 0);
}

long xyuos_rename(const char *oldpath, const char *newpath) {
    return xyuos_syscall3(SYS_RENAME, (long)oldpath, (long)newpath, 0);
}

/* POSIX rename(2). 0 on success, -1 on error. */
int rename(const char *oldpath, const char *newpath) {
    return (int)xyuos_syscall3(SYS_RENAME, (long)oldpath, (long)newpath, 0);
}

/* No shell to run commands through -- always "failed". Present so third-party
 * code that calls system() links. */
int system(const char *cmd) { (void)cmd; return -1; }

key_event_t xyuos_read_event(void) {
    long packed = xyuos_syscall3(SYS_READEVENT, 0, 0, 0);
    key_event_t ev;
    ev.ascii   = (char)(packed & 0xFF);
    ev.code    = (unsigned char)((packed >> 8) & 0xFF);
    ev.mods    = (unsigned char)((packed >> 16) & 0xFF);
    ev.pressed = 1;   // this call blocks for a keystroke; there is no other kind
    return ev;
}

void tty_read_key(struct key_event *ev) {
    long packed = xyuos_syscall3(SYS_READEVENT, 0, 0, 0);
    ev->ascii = (char)(packed & 0xFF);
    ev->code  = (int)((packed >> 8) & 0xFF);
    ev->mods  = (int)((packed >> 16) & 0xFF);
}

void xyuos_termsize(unsigned int *cols, unsigned int *rows) {
    long packed = xyuos_syscall3(SYS_TERMSIZE, 0, 0, 0);
    if (cols) *cols = (unsigned int)((packed >> 16) & 0xFFFF);
    if (rows) *rows = (unsigned int)(packed & 0xFFFF);
}

void _exit(int code) {
    xyuos_syscall3(SYS_EXIT, code, 0, 0);
    for (;;) { }
}
