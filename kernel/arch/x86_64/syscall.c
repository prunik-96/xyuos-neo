#include "syscall.h"
#include "../../kernel/kio.h"
#include "../../kernel/process.h"
#include "../../kernel/shm.h"
#include "../../kernel/bkl.h"
#include "../../kernel/devices.h"
#include "../../mm/vmm.h"
#include "../../mm/pmm.h"
#include "../../fs/vfs.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/framebuffer.h"
#include "../../drivers/power.h"
#include "../../drivers/rtc.h"
#include "smp.h"
#include "../../fs/fat32.h"
#include "../../wm/wm.h"
#include "../../net/net.h"
#include "../../net/nic.h"
#include "../../net/tls.h"
#include "pit.h"
#include "../../gfx/font.h"
#include "../../drivers/audio.h"
#include "../../drivers/mouse.h"

// SYS_USBFS list: copy each FAT32 directory entry into the user's array.
struct usbfs_fill { struct usb_ent *arr; unsigned int max, n; };
static void usbfs_list_cb(const struct fat_dirent *e, void *v) {
    struct usbfs_fill *fc = (struct usbfs_fill *)v;
    if (fc->n >= fc->max) return;
    struct usb_ent *o = &fc->arr[fc->n];
    int i = 0;
    for (; i < 15 && e->name[i]; i++) o->name[i] = e->name[i];
    o->name[i] = 0;
    o->size = e->size;
    o->is_dir = e->is_dir;
    fc->n++;
}
#include <stdint.h>

#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry(void);

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1; // SCE: enable SYSCALL/SYSRET
    wrmsr(MSR_EFER, efer);

    // bits 63:48 = user segment base: SYSRET returns with SS = base+8 and
    // CS = base+16. bits 47:32 = kernel CS (0x08 -> kernel SS=0x10 via SYSCALL).
    //
    // The base carries RPL 3 (0x1B, not 0x18) and that is not cosmetic.
    //
    // SYSRET on AMD sets SS.Selector to base+8 as written, WITHOUT forcing the
    // low two bits to 3 the way it does for CS. With a base of 0x18 the user
    // came back from every syscall holding SS = 0x20 -- the right descriptor,
    // ring 0 in the selector. Nothing complains: SS is not checked again while
    // the program runs, and a program that spends its time blocked in a syscall
    // never notices. But let a timer interrupt land while that program is
    // actually executing, and the CPU pushes SS = 0x20 onto the interrupt
    // frame; the IRETQ that resumes it then refuses to return to ring 3 with a
    // ring-0 stack selector and raises #GP(0x20), inside the kernel, fatally.
    //
    // That is the crash that looked like "the OS falls over when you open a
    // program": the shell survives because it sits in a blocking read, while
    // anything that draws -- the file manager, the editor -- runs long enough
    // in user mode to be interrupted.
    //
    // Putting the 3 in the base makes both derived selectors correct on every
    // CPU: SS = 0x1B+8 = 0x23, CS = 0x1B+16 = 0x2B. This is exactly why Linux
    // stores __USER32_CS (RPL 3 included) in STAR rather than a bare index.
    uint64_t star = ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200); // clear IF on syscall entry
}

// The pane the calling process may use, or NULL if it has none (boot-time
// programs, anything started outside the WM).
//
// A pane belongs to a process TREE, not a single process: the shell owns it,
// and a program the shell started and is waiting on uses the same pane. That
// is what makes `fm` or an editor able to take over the window without a
// handover protocol -- and the parent, blocked in wait(), cannot compete for
// it. The walk is over the caller's own ancestry, so a process still cannot
// name somebody else's pane; there is no handle to forge.
static struct pane *my_pane(void) {
    process_t *me = process_current();
    for (int hops = 0; me && hops < 8; hops++) {
        struct pane *p = wm_pane_for_pid(me->pid);
        if (p) return p;
        if (me->parent_pid <= 0) break;
        me = process_by_pid(me->parent_pid);
    }
    return 0;
}

static uint64_t syscall_do(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                           struct syscall_frame *f) {
    (void)f;
    switch (num) {
        case SYS_WRITE: {
            const char *buf = (const char *)(uintptr_t)a1;
            uint64_t len = a2;
            (void)a3;
            uint64_t addr = (uint64_t)(uintptr_t)buf;
            if (!vmm_user_range_ok(addr, len)) {
                return (uint64_t)-1;
            }

            // Redirected output (a file or a pipe) wins over the pane. A pipe
            // write may legitimately return -1 (broken pipe) -- that is NOT a
            // reason to splatter across the window, so only the TERMINAL
            // sentinel falls through.
            long n = process_write_stdout(buf, len);
            if (n != STREAM_TERMINAL) return (uint64_t)n;

            // A process that owns a pane writes into it -- that is what makes
            // printf() work inside a window. Everything else (boot programs,
            // tests) still goes to the kernel console and the serial log.
            struct pane *p = my_pane();
            if (p) {
                for (uint64_t i = 0; i < len; i++) pane_putc(p, buf[i]);
                wm_mark_dirty();
                return len;
            }
            for (uint64_t i = 0; i < len; i++) {
                kprintf("%c", buf[i]);
            }
            return len;
        }
        case SYS_EXIT: {
            process_notify_exit((int)a1);
            // never returns: unwinds straight back into process_run_image().
            for (;;) { }
        }
        case SYS_OPEN: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_open((const char *)(uintptr_t)a1);
        }
        case SYS_READ: {
            if (!vmm_user_range_ok(a2, a3)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_read((int)a1, (void *)(uintptr_t)a2, (uint32_t)a3);
        }
        case SYS_CLOSE: {
            vfs_close((int)a1);
            return 0;
        }
        case SYS_LISTDIR: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            if (!vmm_user_range_ok(a2, a3)) return (uint64_t)-1;
            return vfs_list_dir((const char *)(uintptr_t)a1, (char *)(uintptr_t)a2, (uint32_t)a3);
        }
        case SYS_READKEY: {
            // Poll-then-sleep rather than keyboard_getchar(): that one spins on
            // `sti; hlt` inside the kernel, which does not deadlock but does
            // stall every other process, because the timer never preempts
            // ring 0. Sleeping hands the CPU to somebody else instead.
            char c = 0;
            // A signal with a handler breaks the wait. The value returned
            // here is thrown away: the call is put back and happens again
            // once the handler is done, so the program still ends up with the
            // key it asked for.
            while (!keyboard_poll(&c)) if (process_block_on_key()) break;
            return (uint64_t)(uint8_t)c;
        }
        case SYS_SIGNAL: {
            switch (a1) {
                case SIGOP_HANDLER: return signal_set_handler((int)a2, a3);
                case SIGOP_SEND:
                    return (uint64_t)(int64_t)signal_send((int)a2, (int)a3);
                case SIGOP_RETURN:  return signal_return(f, a2);
                case SIGOP_TRAMP:
                    return (uint64_t)(int64_t)signal_set_trampoline(a2);
                case SIGOP_ALARM:
                    return (uint64_t)(int64_t)signal_alarm(a2);
                case SIGOP_MASK:
                    return (uint64_t)signal_mask((int)a2, (uint32_t)a3);
                default: return (uint64_t)-1;
            }
        }
        case SYS_SHM: {
            switch (a1) {
                case SHMOP_GET: {
                    if (!vmm_user_range_ok(a2, sizeof(struct shm_req)))
                        return (uint64_t)-1;
                    const struct shm_req *rq =
                        (const struct shm_req *)(uintptr_t)a2;
                    // Read out before anything else runs: they are the
                    // caller's memory and must not be looked at twice.
                    int key = rq->key, flags = rq->flags;
                    uint64_t size = rq->size;
                    return (uint64_t)(int64_t)shm_get(key, size, flags);
                }
                case SHMOP_ATTACH: return shm_attach((int)a2, (int)a3);
                case SHMOP_DETACH:
                    return (uint64_t)(int64_t)shm_detach(a2);
                case SHMOP_CTL:
                    return (uint64_t)(int64_t)shm_ctl((int)a2, (int)a3);
                case SHMOP_SIZE:   return shm_size((int)a2);
                default: return (uint64_t)-1;
            }
        }
        case SYS_HYPER: {
            wm_request_split();
            return 0;
        }
        case SYS_TTYCTL: {
            struct pane *p = my_pane();
            if (!p) return (uint64_t)-1;
            switch (a1) {
                case TTY_CLEAR:      pane_clear(p); break;
                case TTY_MOVE:       pane_move(p, (uint32_t)a2, (uint32_t)a3); break;
                case TTY_SETCOLOR:   pane_set_color(p, (uint8_t)a2, (uint8_t)a3); break;
                case TTY_RESETCOLOR: pane_reset_color(p); break;
                case TTY_ERASELINE:  pane_erase_line(p); break;
                case TTY_SIZE:       return ((uint64_t)p->cols << 16) | p->rows;
                case TTY_GETCUR:     return ((uint64_t)p->cursor_row << 16) | p->cursor_col;
                default: return (uint64_t)-1;
            }
            wm_mark_dirty();
            return 0;
        }
        case SYS_CREATE: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_create((const char *)(uintptr_t)a1);
        }
        case SYS_WRITEFILE: {
            if (!vmm_user_range_ok(a2, a3)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_write((int)a1, (const void *)(uintptr_t)a2, (uint32_t)a3);
        }
        case SYS_UNLINK: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_unlink((const char *)(uintptr_t)a1);
        }
        case SYS_MKDIR: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_mkdir((const char *)(uintptr_t)a1);
        }
        case SYS_RENAME: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            if (!vmm_user_range_ok(a2, 1)) return (uint64_t)-1;
            return (uint64_t)(int64_t)vfs_rename((const char *)(uintptr_t)a1,
                                                 (const char *)(uintptr_t)a2);
        }
        case SYS_READEVENT: {
            // Zeroed because a signal can break the wait below before any
            // event has been read. What is returned then is discarded: the
            // call is put back and happens again after the handler.
            struct kbd_event ev = (struct kbd_event){0};
            struct pane *p = my_pane();
            if (p) {
                // Pane owners read their OWN queue, filled by the WM only while
                // they hold focus. That is what keeps a background pane from
                // stealing the keyboard.
                while (!wm_pane_pop_event(p, &ev)) if (process_block_on_key()) break;
            } else {
                // No window: read the raw ring, which now carries both edges.
                // A blocking "give me a keystroke" means a key going down.
                for (;;) {
                    if (!keyboard_poll_event(&ev)) {
                        if (process_block_on_key()) break;
                        continue;
                    }
                    if (ev.pressed) break;
                }
            }
            return ((uint64_t)ev.mods << 16) | ((uint64_t)ev.code << 8) | (uint64_t)(uint8_t)ev.ascii;
        }
        case SYS_TERMSIZE: {
            struct pane *p = my_pane();
            if (p) return ((uint64_t)p->cols << 16) | (uint64_t)p->rows;
            uint32_t cols = 0, rows = 0;
            fb_console_size(&cols, &rows);
            return ((uint64_t)cols << 16) | (uint64_t)rows;
        }
        case SYS_SEEK: {
            return (uint64_t)vfs_seek((int)a1, (int64_t)a2, (int)a3);
        }
        case SYS_STAT: {
            if (!vmm_user_range_ok(a1, 1)) return (uint64_t)-1;
            if (!vmm_user_range_ok(a2, 1)) return (uint64_t)-1;
            // `struct xyuos_stat` in libc/include/unistd.h mirrors this layout
            // field for field; keep the two in step.
            return (uint64_t)(int64_t)vfs_stat((const char *)(uintptr_t)a1,
                                               (struct vfs_stat *)(uintptr_t)a2);
        }
        case SYS_TRUNCATE: {
            return (uint64_t)(int64_t)vfs_ftruncate((int)a1, (uint32_t)a2);
        }
        case SYS_SBRK: {
            return process_sbrk((int64_t)a1);
        }
        case SYS_SPAWN: {
            // Pointer validation happens inside, where the strings are copied
            // out of user space before the child address space is built.
            return (uint64_t)(int64_t)process_spawn_user(a1, a2, (int)a3);
        }
        case SYS_WAIT: {
            return (uint64_t)(int64_t)process_wait((int)(int64_t)a1);
        }
        case SYS_SETTING: {
            if (a1 == SETOP_PALETTE) {
                if (a3 < sizeof(struct ui_palette)) return (uint64_t)-1;
                if (!vmm_user_range_ok(a2, sizeof(struct ui_palette)))
                    return (uint64_t)-1;
                wm_palette((struct ui_palette *)(uintptr_t)a2);
                return sizeof(struct ui_palette);
            }
            if (a1 == SETOP_THEME_NAME) {
                if (!vmm_user_range_ok(a3, 32)) return (uint64_t)-1;
                const char *n = wm_theme_name((int)a2);
                char *out = (char *)(uintptr_t)a3;
                int i = 0;
                while (n[i] && i < 31) { out[i] = n[i]; i++; }
                out[i] = 0;
                return (uint64_t)i;
            }
            int set = (a1 == SETOP_SET);
            switch (a2) {
                case SET_THEME:
                    if (set) wm_theme_set((int)a3);
                    return (uint64_t)wm_theme_get();
                case SET_THEME_COUNT:
                    return (uint64_t)wm_theme_count();
                case SET_KEY_DELAY:
                    if (set) keyboard_repeat_config((int)a3, keyboard_repeat_rate());
                    return (uint64_t)keyboard_repeat_delay();
                case SET_KEY_RATE:
                    if (set) keyboard_repeat_config(keyboard_repeat_delay(), (int)a3);
                    return (uint64_t)keyboard_repeat_rate();
                case SET_MOUSE_SPEED:
                    if (set) mouse_set_speed((int)a3);
                    return (uint64_t)mouse_speed();
                case SET_DBLCLICK:
                    if (set) wm_set_dblclick_ms((int)a3);
                    return (uint64_t)wm_dblclick_ms();
                default:
                    return (uint64_t)-1;
            }
        }
        case SYS_SYSINFO: {
            if (!vmm_user_range_ok(a2, a3)) return (uint64_t)-1;
            void *buf = (void *)(uintptr_t)a2;

            if (a1 == SI_MEM) {
                if (a3 < sizeof(struct si_mem)) return (uint64_t)-1;
                struct si_mem *m = (struct si_mem *)buf;
                m->page_size = PAGE_SIZE;
                m->total_frames = pmm_total_frame_count();
                m->free_frames = pmm_free_frame_count();
                return sizeof(*m);
            }
            if (a1 == SI_PROCS) {
                int max = (int)(a3 / sizeof(struct si_proc));
                if (max <= 0) return (uint64_t)-1;
                int n = process_list((struct si_proc *)buf, max);
                return (uint64_t)(n * (int)sizeof(struct si_proc));
            }
            if (a1 == SI_DEVICES) {
                int max = (int)(a3 / sizeof(struct si_dev));
                if (max <= 0) return (uint64_t)-1;
                int n = device_list((struct si_dev *)buf, max);
                return (uint64_t)(n * (int)sizeof(struct si_dev));
            }
            if (a1 == SI_UNAME) {
                const char *s = "xyuOS Neo x86_64";
                uint64_t i = 0;
                char *out = (char *)buf;
                while (s[i] && i + 1 < a3) { out[i] = s[i]; i++; }
                out[i] = '\0';
                return i;
            }
            return (uint64_t)-1;
        }
        case SYS_SLEEP: {
            process_sleep_ms(a1);
            return 0;
        }
        case SYS_KILL: {
            return (uint64_t)(int64_t)process_kill((int)a1);
        }
        case SYS_PIPE: {
            return (uint64_t)(int64_t)pipe_create();
        }
        case SYS_PIPECLOSE: {
            pipe_close((int)a1);
            return 0;
        }
        case SYS_POWER: {
            if (a1 == POWER_REBOOT) power_reboot();   // does not return
            else if (a1 == POWER_OFF) power_off();     // returns only if it fails
            else if (a1 == POWER_SLEEP) {
                // Lightweight sleep: blank the screen and halt until a key wakes
                // us, then repaint the desktop. (Not ACPI S3 -- that needs a
                // wake vector and full device-state save/restore.)
                //
                // Wake off the ASCII key ring, NOT the event ring: wm_route_input()
                // (called from the keyboard IRQ) drains the event ring, so a loop
                // on keyboard_poll_event() would never see the wake key. The ASCII
                // ring is filled by the same keypress and nothing else consumes it
                // while the shell is parked here.
                fb_fill_rect(0, 0, fb_get_width(), fb_get_height(), 0x00000000);
                fb_present();
                keyboard_flush();
                char c;
                // Waiting with the kernel lock let go of. The keypress that
                // ends this arrives on the bootstrap core, and its handler
                // needs the lock: on any other core, `hlt` with the lock held
                // would wait for a key that could never be delivered.
                while (!keyboard_poll(&c)) bkl_wait_interrupt();
                keyboard_flush();   // drop the wake key so it doesn't hit the shell
                wm_refresh();
            }
            return 0;
        }
        case SYS_TIME: {
            if (!vmm_user_range_ok(a1, sizeof(struct sys_tm))) return (uint64_t)-1;
            struct rtc_time t;
            rtc_read(&t);
            struct sys_tm *out = (struct sys_tm *)(uintptr_t)a1;
            out->sec = t.sec; out->min = t.min; out->hour = t.hour;
            out->day = t.day; out->mon = t.mon; out->year = t.year;
            return 0;
        }
        case SYS_USBFS: {
            if (!vmm_user_range_ok(a1, sizeof(struct usbfs_req))) return (uint64_t)-1;
            struct usbfs_req *rq = (struct usbfs_req *)(uintptr_t)a1;
            rq->result = -1;
            if (rq->op == USBFS_MOUNT) { rq->result = fat32_automount(); return 0; }
            if (!fat32_mounted()) return 0;
            if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->path, 1)) return (uint64_t)-1;
            if (rq->op == USBFS_LIST) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf,
                                       (uint64_t)rq->len * sizeof(struct usb_ent)))
                    return (uint64_t)-1;
                struct usbfs_fill fc = { (struct usb_ent *)rq->buf, rq->len, 0 };
                fat32_list(rq->path, usbfs_list_cb, &fc);
                rq->result = (int)fc.n;
            } else if (rq->op == USBFS_READ) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, rq->len))
                    return (uint64_t)-1;
                rq->result = (int)fat32_read(rq->path, 0, rq->buf, rq->len);
            } else if (rq->op == USBFS_WRITE) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, rq->len))
                    return (uint64_t)-1;
                rq->result = (int)fat32_write(rq->path, rq->buf, rq->len);
            }
            return 0;
        }
        case SYS_NET: {
            if (!vmm_user_range_ok(a1, sizeof(struct net_req))) return (uint64_t)-1;
            struct net_req *rq = (struct net_req *)(uintptr_t)a1;
            rq->result = -1;

            if (rq->op == NET_UP) { rq->result = net_up(); return 0; }

            if (rq->op == NET_STATUS) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf,
                                       sizeof(struct net_status))) return (uint64_t)-1;
                struct net_status *st = (struct net_status *)rq->buf;
                for (unsigned i = 0; i < sizeof *st; i++) ((char *)st)[i] = 0;
                // nic_init() is idempotent and does not wait for anything, so
                // asking for status is cheap even before the link is up.
                int have = nic_init();
                if (have) {
                    const char *nm = nic_name();
                    int i = 0;
                    for (; nm && nm[i] && i < 15; i++) st->driver[i] = nm[i];
                    const uint8_t *m = nic_mac();
                    if (m) for (int j = 0; j < 6; j++) st->mac[j] = m[j];
                    st->link = nic_link() ? 1 : 0;
                }
                st->up = net_is_up() ? 1 : 0;
                if (st->up) net_config(&st->ip, &st->gw, &st->mask, &st->dns);
                rq->result = have;
                return 0;
            }

            if (rq->op == NET_CONFIG) {
                if (!net_is_up()) return 0;
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf,
                                       sizeof(struct net_info))) return (uint64_t)-1;
                struct net_info *ni = (struct net_info *)rq->buf;
                net_config(&ni->ip, &ni->gw, &ni->mask, &ni->dns);
                rq->result = 0;
                return 0;
            }

            if (rq->op == NET_LOG) {
                if (rq->arg) { net_log_clear(); rq->result = 0; return 0; }
                unsigned bytes = rq->len * (unsigned)sizeof(struct net_log_ent);
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, bytes))
                    return (uint64_t)-1;
                rq->result = net_log_read((struct net_log_ent *)rq->buf,
                                          (int)rq->len);
                return 0;
            }

            // The remaining ops need the link; bring it up on first use.
            if (!net_is_up() && !net_up()) return 0;

            if (rq->op == NET_PING) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->a, 1)) return (uint64_t)-1;
                uint32_t ip;
                if (!net_resolve(rq->a, &ip)) { rq->result = -1; return 0; }
                uint32_t rtt = 0;
                rq->result = net_ping(ip, &rtt) ? (int)rtt : -1;
            } else if (rq->op == NET_RESOLVE) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->a, 1)) return (uint64_t)-1;
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, 4)) return (uint64_t)-1;
                uint32_t ip;
                if (net_resolve(rq->a, &ip)) {
                    *(uint32_t *)rq->buf = ip;
                    rq->result = 0;
                }
            } else if (rq->op == NET_FSTART) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->a, 1)) return (uint64_t)-1;
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->b, 1)) return (uint64_t)-1;
                const char *body = 0;
                int blen = 0;
                if (rq->arg & NET_HTTPGET_POST) {
                    if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, rq->len))
                        return (uint64_t)-1;
                    body = (const char *)rq->buf;
                    blen = (int)rq->len;
                }
                const char *xhdr = 0;
                if (rq->hdr && rq->hdrlen) {
                    if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->hdr, rq->hdrlen))
                        return (uint64_t)-1;
                    xhdr = rq->hdr;
                }
                rq->result = net_fetch_start(rq->a, rq->b,
                                             (uint16_t)(rq->arg & 0xFFFF),
                                             (rq->arg & NET_HTTPGET_TLS) ? 1 : 0,
                                             (rq->arg & NET_HTTPGET_RAW) ? 1 : 0,
                                             body, blen, xhdr,
                                             (int)(xhdr ? rq->hdrlen : 0));
            } else if (rq->op == NET_FPOLL) {
                // The slot travels in `len`, the progress comes back in
                // `arg`: a poll uses neither for anything else.
                int got = 0;
                rq->result = net_fetch_poll((int)rq->len, &got);
                rq->arg = (unsigned)got;
            } else if (rq->op == NET_FTAKE) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, rq->len))
                    return (uint64_t)-1;
                rq->result = net_fetch_take((int)rq->arg,
                                            (char *)rq->buf, (int)rq->len);
            } else if (rq->op == NET_FCANCEL) {
                net_fetch_cancel((int)rq->arg);
                rq->result = 0;
            } else if (rq->op == NET_FSLOTS) {
                rq->result = net_fetch_slots();
            } else if (rq->op == NET_HTTPGET) {
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->a, 1)) return (uint64_t)-1;
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->b, 1)) return (uint64_t)-1;
                if (!vmm_user_range_ok((uint64_t)(uintptr_t)rq->buf, rq->len))
                    return (uint64_t)-1;
                uint32_t ip;
                if (!net_resolve(rq->a, &ip)) { rq->result = -1; return 0; }
                uint16_t port = (uint16_t)(rq->arg & 0xFFFF);
                int raw = (rq->arg & NET_HTTPGET_RAW) ? 1 : 0;
                rq->result = (rq->arg & NET_HTTPGET_TLS)
                    ? tls_https_get(rq->a, ip, port, rq->b, (char *)rq->buf,
                                    (int)rq->len, raw, 0, 0, 0)
                    : net_http_get(rq->a, ip, port, rq->b, (char *)rq->buf,
                                   (int)rq->len, raw, 0, 0, 0);
            }
            return 0;
        }
        case SYS_THREAD: {
            if (a1 == THREAD_OP_CREATE) {
                return (uint64_t)(int64_t)process_thread_create(a2, a3);
            }
            if (a1 == THREAD_OP_EXIT) {
                process_thread_exit((int)a2);   // does not return
                return 0;
            }
            if (a1 == THREAD_OP_JOIN) {
                return (uint64_t)(int64_t)process_thread_join((int)a2);
            }
            if (a1 == THREAD_OP_SELF) {
                process_t *me = process_current();
                return me ? (uint64_t)(int64_t)me->pid : (uint64_t)-1;
            }
            return (uint64_t)-1;
        }
        case SYS_VM: {
            // The dispatcher has three arguments and protect needs four
            // things said, so the protection travels in the top of the op --
            // it is three bits, and growing the dispatcher for one caller
            // would be the tail wagging the dog.
            uint64_t op = a1 & 0xFF;
            uint32_t prot = (uint32_t)(a1 >> 8);
            if (op == VM_OP_MAP)     return vmm_mmap(a2, prot);
            if (op == VM_OP_UNMAP)   return (uint64_t)(int64_t)vmm_munmap(a2, a3);
            if (op == VM_OP_PROTECT) return (uint64_t)(int64_t)
                                            vmm_mprotect(a2, a3, prot);
            return (uint64_t)-1;
        }
        case SYS_GFX: {
            struct pane *p = my_pane();
            if (!p) return (uint64_t)-1;
            if (a1 == GFX_BLIT) {
                int w = (int)((a3 >> 16) & 0xFFFF);
                int h = (int)(a3 & 0xFFFF);
                if (w <= 0 || h <= 0) return (uint64_t)-1;
                if (!vmm_user_range_ok(a2, (uint64_t)w * h * 4)) return (uint64_t)-1;
                return (uint64_t)(int64_t)wm_pane_blit(p, (const uint32_t *)(uintptr_t)a2, w, h);
            } else if (a1 == GFX_INFO) {
                int w = 0, h = 0;
                wm_pane_interior(p, &w, &h);
                return ((uint64_t)w << 16) | (uint64_t)h;
            } else if (a1 == GFX_END) {
                wm_pane_gfx_end(p);
                return 0;
            }
            return (uint64_t)-1;
        }
        case SYS_POLLEVENT: {
            struct pane *p = my_pane();
            if (a1 == POLLEV_KEYUP) {
                // Asking for the whole keyboard rather than just what was
                // typed. Only meaningful for a program with a window.
                if (!p) return (uint64_t)-1;
                p->keys_raw = (a2 != 0);
                {   // Remember who asked, so the mode dies with them.
                    process_t *me = process_current();
                    p->keys_raw_pid = (a2 && me) ? me->pid : 0;
                }
                return 0;
            }
            struct kbd_event ev;
            int got = p ? wm_pane_pop_event(p, &ev) : keyboard_poll_event(&ev);
            if (!got) return (uint64_t)-1;
            return ((uint64_t)(ev.pressed ? 1 : 0) << 24) |
                   ((uint64_t)ev.mods << 16) | ((uint64_t)ev.code << 8) |
                   (uint64_t)(uint8_t)ev.ascii;
        }
        case SYS_FONT: {
            if (!font_ready()) return (uint64_t)-1;
            uint32_t cw = font_cell_w(), chh = font_cell_h();
            if (a1 == FONT_INFO) return ((uint64_t)cw << 16) | chh;
            if (a1 == FONT_ATLAS) {
                uint64_t cell = (uint64_t)cw * chh, need = cell * FONT_SLOTS;
                if (a3 < need) return (uint64_t)-1;
                if (!vmm_user_range_ok(a2, need)) return (uint64_t)-1;
                uint8_t *dst = (uint8_t *)(uintptr_t)a2;
                for (int ch = 0; ch < FONT_SLOTS; ch++) {
                    const uint8_t *g = font_glyph_slot(ch);
                    for (uint64_t k = 0; k < cell; k++)
                        dst[ch * cell + k] = g ? g[k] : 0;
                }
                return need;
            }
            return (uint64_t)-1;
        }
        case SYS_PROCCTL: {
            int pid = (int)a2;
            if (a1 == PC_KILL) return (uint64_t)(int64_t)process_kill(pid);
            if (a1 == PC_STOP) return (uint64_t)(int64_t)process_suspend(pid);
            if (a1 == PC_CONT) return (uint64_t)(int64_t)process_resume(pid);
            return (uint64_t)-1;
        }
        case SYS_SPAWNWIN: {
            if (!vmm_user_range_ok(a1, sizeof(struct winspawn))) return (uint64_t)-1;
            struct winspawn *rq = (struct winspawn *)(uintptr_t)a1;
            // The strings live in the caller's address space, which is the one
            // mapped right now; wm_spawn_window copies what it needs before
            // the child's address space is switched in.
            return (uint64_t)(int64_t)wm_spawn_window(rq->path, rq->arg);
        }
        case SYS_MSGBOX: {
            if (!vmm_user_range_ok(a1, sizeof(struct msgbox_req))) return (uint64_t)-1;
            struct msgbox_req *rq = (struct msgbox_req *)(uintptr_t)a1;
            // The strings live in the caller's address space, which is the one
            // that is mapped right now -- wm_message_box() copies them before
            // it returns, so they need not outlive this call.
            wm_message_box(rq->kind, rq->code, rq->title, rq->text, rq->detail);
            return 0;
        }
        case SYS_AUDIO: {
            if (a1 == AU_INFO)   return (uint64_t)audio_ready();
            if (a1 == AU_QUEUED) return (uint64_t)(int64_t)audio_queued();
            if (a1 == AU_STOP)   { audio_stop(); return 0; }
            if (a1 == AU_VOLUME) {
                if ((int64_t)a2 >= 0) audio_set_volume((int)a2);
                return (uint64_t)audio_volume();
            }
            if (a1 == AU_WRITE) {
                if (!vmm_user_range_ok(a2, a3 * 2 * AUDIO_CH)) return (uint64_t)-1;
                return (uint64_t)(int64_t)audio_write((const int16_t *)(uintptr_t)a2,
                                                      (int)a3);
            }
            return (uint64_t)-1;
        }
        case SYS_POLLMOUSE: {
            if (!vmm_user_range_ok(a1, sizeof(struct umouse))) return (uint64_t)-1;
            struct pane *p = my_pane();
            if (!p) return (uint64_t)-1;
            struct pane_mouse pm;
            if (!wm_pane_pop_mouse(p, &pm)) return (uint64_t)-1;
            struct umouse *u = (struct umouse *)(uintptr_t)a1;
            u->x = pm.x; u->y = pm.y;
            u->buttons = pm.buttons;
            u->pressed = pm.pressed;
            u->released = pm.released;
            u->wheel = pm.wheel;
            u->drag = pm.drag;
            return 0;
        }
        case SYS_DRAG: {
            if (a1 == DRAG_CANCEL) { wm_drag_cancel(); return 0; }
            if (a1 == DRAG_BEGIN) {
                if (!a2) return (uint64_t)-1;
                return (uint64_t)(int64_t)wm_drag_begin((const char *)(uintptr_t)a2,
                                                        (const char *)(uintptr_t)a3);
            }
            if (a1 == DRAG_TAKE) {
                if (!vmm_user_range_ok(a2, a3)) return (uint64_t)-1;
                struct pane *p = my_pane();
                if (!p) return (uint64_t)-1;
                return (uint64_t)(int64_t)wm_drop_take(p, (char *)(uintptr_t)a2,
                                                       (int)a3);
            }
            return (uint64_t)-1;
        }
        case SYS_UPTIME: {
            return pit_get_ticks() * 10;   // 100 Hz tick -> milliseconds
        }
        case SYS_SMP: {
            if (!vmm_user_range_ok(a2, sizeof(struct smp_result))) return (uint64_t)-1;
            smp_bench(a1, (struct smp_result *)(uintptr_t)a2);
            return 0;
        }
        case SYS_SPAWN2: {
            return (uint64_t)(int64_t)process_spawn_user2(a1);
        }
        case SYS_READSTD: {
            if (!vmm_user_range_ok(a1, a2)) return (uint64_t)-1;
            long n = process_read_stdin((void *)(uintptr_t)a1, a2);
            if (n != STREAM_TERMINAL) return (uint64_t)n;  // file or pipe (0 = EOF)

            if (a2 == 0) return 0;
            struct pane *p = my_pane();
            if (!p) return 0;                 // no terminal: end of input

            // Terminal input is gathered a LINE at a time (canonical mode).
            // The kernel echoes as it goes and handles backspace, because
            // nothing else does -- a program calling scanf() would otherwise
            // leave the user typing blind.
            if (p->in_pos >= p->in_len) {
                p->in_len = 0;
                p->in_pos = 0;
                for (;;) {
                    struct kbd_event ev;
                    // This one is NOT interruptible. The half-typed line lives
                    // in the pane, and putting the call back would start it
                    // again from nothing -- the person would watch their own
                    // typing disappear. The handler runs when the line is done.
                    while (!wm_pane_pop_event(p, &ev))
                        if (process_block_on_key()) signal_no_restart();

                    if (ev.code == KEY_ENTER) {
                        if (p->in_len < PANE_INPUT_MAX) p->in_line[p->in_len++] = '\n';
                        pane_putc(p, '\n');
                        wm_mark_dirty();
                        break;
                    }
                    if (ev.code == KEY_BKSP) {
                        if (p->in_len > 0) {
                            p->in_len--;
                            pane_putc(p, '\b');
                            wm_mark_dirty();
                        }
                        continue;
                    }
                    // Ctrl+D on an empty line is end of input, like a tty.
                    if (ev.code == KEY_CHAR && (ev.mods & KBD_MOD_CTRL) &&
                        (ev.ascii == 'd' || ev.ascii == 'D')) {
                        if (p->in_len == 0) return 0;
                        break;
                    }
                    if (ev.code == KEY_CHAR && ev.ascii >= 32 && ev.ascii < 127) {
                        if (ev.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_SUPER)) continue;
                        if (p->in_len < PANE_INPUT_MAX - 1) {
                            p->in_line[p->in_len++] = ev.ascii;
                            pane_putc(p, ev.ascii);
                            wm_mark_dirty();
                        }
                    }
                }
            }

            uint64_t avail = p->in_len - p->in_pos;
            uint64_t want = (a2 < avail) ? a2 : avail;
            char *dst = (char *)(uintptr_t)a1;
            for (uint64_t i = 0; i < want; i++) dst[i] = p->in_line[p->in_pos + i];
            p->in_pos += want;
            return want;
        }
        default:
            return (uint64_t)-1;
    }
}

// Every syscall goes in and out through here, which is the only place that
// sees both ends of one: the safe point before it runs, and the frame it is
// about to return through.
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          struct syscall_frame *f) {
    // The kernel is entered here, so the kernel lock is taken here: nothing
    // below this line runs on two cores at once. See bkl.h.
    bkl_enter();

    // Anything pending that needs no user code happens BEFORE the call the
    // process was about to make. This is a safe point: no fd is open, no
    // buffer half written. Never returns when what is pending ends it.
    signal_check();

    uint64_t ret = syscall_do(num, a1, a2, a3, f);

    // And on the way out, where there is a frame to bend, a handler can run.
    signal_on_syscall_return(f, num, ret);

    // Given back here, in C, because -- unlike an interrupt -- a system call
    // that comes back at all comes back to the process that made it. The
    // stack under this frame belongs to a process RUNNING on this core, and
    // no other core will touch it. The ways out that DO leave a stack behind
    // (blocking, exiting) never return here; they go through the scheduler.
    bkl_exit();
    return ret;
}
