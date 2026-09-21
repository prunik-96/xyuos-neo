/* taskmgr -- the task manager.
 *
 * `ps` prints a snapshot. The interesting question a task manager answers is
 * not "what is running" but "what is eating the machine", and that is a
 * derivative: you cannot read it out of one sample. So this keeps the tick
 * counter from the previous refresh per pid and shows the difference over the
 * interval -- the same reason every real task manager's first row of numbers
 * is wrong until the second refresh.
 *
 * End task and Suspend are deliberately different operations. Killing a
 * process that is merely busy loses its work; suspending it stops it being
 * scheduled while keeping its memory and its windows, so you can look at
 * whatever else is wrong and then let it go again. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xyuos_syscall.h"
#include "gui.h"

#define MAXPROC 64
#define TOOL_H  40
#define STAT_H  26
#define HEAD_H  26
#define SBW     14

static struct si_proc procs[MAXPROC];
static int nproc;

/* Previous sample, so a rate can be computed. */
static int                prev_pid[MAXPROC];
static unsigned long long prev_ticks[MAXPROC];
static int                nprev;
static unsigned           prev_ms;

static int  cpu_pct[MAXPROC];
static int  sel = -1, top, hover = -1;
static char status[180];

static struct si_mem mem;

static const char *state_name(int st, int stopped) {
    if (stopped) return "suspended";
    switch (st) {
        case 1: return "ready";
        case 2: return "running";
        case 3: return "blocked";
        case 4: return "exited";
        default: return "?";
    }
}

static void refresh(void) {
    int keep_pid = (sel >= 0 && sel < nproc) ? procs[sel].pid : -1;

    long n = xyuos_sysinfo(SI_PROCS, procs, sizeof procs);
    nproc = (n > 0) ? (int)(n / (long)sizeof(struct si_proc)) : 0;
    xyuos_sysinfo(SI_MEM, &mem, sizeof mem);

    unsigned now = uptime_ms();
    unsigned dt = now - prev_ms;

    for (int i = 0; i < nproc; i++) {
        cpu_pct[i] = 0;
        for (int k = 0; k < nprev; k++) {
            if (prev_pid[k] != procs[i].pid) continue;
            if (procs[i].ticks < prev_ticks[k] || !dt) break;
            unsigned long long d = procs[i].ticks - prev_ticks[k];
            /* One tick is 10 ms of CPU. */
            cpu_pct[i] = (int)(d * 10 * 100 / dt);
            if (cpu_pct[i] > 100) cpu_pct[i] = 100;
            break;
        }
    }

    nprev = nproc;
    for (int i = 0; i < nproc; i++) {
        prev_pid[i] = procs[i].pid;
        prev_ticks[i] = procs[i].ticks;
    }
    prev_ms = now;

    sel = -1;
    if (keep_pid >= 0)
        for (int i = 0; i < nproc; i++) if (procs[i].pid == keep_pid) sel = i;
}

/* --- layout --------------------------------------------------------------- */

static int row_h(gui_t *g) { return g->fh + 10; }

static void list_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = TOOL_H + HEAD_H;
    *w = g->w;
    *h = g->h - TOOL_H - HEAD_H - STAT_H;
    if (*h < 0) *h = 0;
}

static int visible_rows(gui_t *g) {
    int x, y, w, h;
    list_rect(g, &x, &y, &w, &h);
    int r = h / row_h(g);
    return r < 1 ? 1 : r;
}

/* Column x positions, as fractions of the width, so the table survives a
 * resize without a layout pass. */
static void columns(gui_t *g, int *pid, int *name, int *cpu, int *state, int *win) {
    int w = g->w - SBW;
    *pid   = 12;
    *name  = 12 + g->fw * 7;
    *cpu   = w * 52 / 100;
    *state = w * 66 / 100;
    *win   = w * 86 / 100;
}

static const char *btn_label[] = { "End task", "Suspend", "Resume", "Refresh" };
#define NBTN ((int)(sizeof btn_label / sizeof btn_label[0]))

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 11, gap = 6;
    *w = bw; *h = TOOL_H - 12; *x = 8 + i * (bw + gap); *y = 6;
}

static void draw(gui_t *g) {
    gui_clear(g, GC_WIN);

    int cpid, cname, ccpu, cstate, cwin;
    columns(g, &cpid, &cname, &ccpu, &cstate, &cwin);

    /* toolbar */
    gui_vgrad(g, 0, 0, g->w, TOOL_H, GC_PANEL, GC_BAR);
    gui_fill(g, 0, TOOL_H - 1, g->w, 1, GC_EDGE);
    for (int i = 0; i < NBTN; i++) {
        int x, y, w, h;
        btn_rect(g, i, &x, &y, &w, &h);
        int st = gui_in(g->mx, g->my, x, y, w, h) ? GB_HOVER : GB_NORMAL;
        if (i < 3 && sel < 0) st = GB_OFF;
        if (i == 1 && sel >= 0 && procs[sel].stopped) st = GB_OFF;
        if (i == 2 && sel >= 0 && !procs[sel].stopped) st = GB_OFF;
        gui_button(g, x, y, w, h, btn_label[i], st);
    }

    /* column headings */
    gui_vgrad(g, 0, TOOL_H, g->w, HEAD_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, TOOL_H + HEAD_H - 1, g->w, 1, GC_EDGE);
    int hy = TOOL_H + (HEAD_H - g->fh) / 2;
    gui_text(g, cpid, hy, "PID", GC_DIM);
    gui_text(g, cname, hy, "Name", GC_DIM);
    gui_text(g, ccpu, hy, "CPU", GC_DIM);
    gui_text(g, cstate, hy, "State", GC_DIM);
    gui_text(g, cwin, hy, "Window", GC_DIM);

    /* rows */
    int lx, ly, lw, lh;
    list_rect(g, &lx, &ly, &lw, &lh);
    gui_fill(g, lx, ly, lw, lh, GC_PANEL);
    int rh = row_h(g), vis = visible_rows(g);
    int need_sb = nproc > vis;
    int inner = lw - (need_sb ? SBW : 0);

    for (int i = 0; i < vis && top + i < nproc; i++) {
        int idx = top + i, y = ly + i * rh;
        struct si_proc *p = &procs[idx];
        unsigned bg = (idx & 1) ? GC_ALT : GC_PANEL;
        if (idx == hover) bg = GC_HOT;
        if (idx == sel)   bg = GC_SEL;
        gui_fill(g, lx, y, inner, rh, bg);
        if (idx == sel) gui_fill(g, lx, y, 3, rh, GC_ACCENT);

        int ty = y + (rh - g->fh) / 2;
        char t[64];

        snprintf(t, sizeof t, "%d", p->pid);
        gui_text(g, cpid, ty, t, GC_DIM);
        gui_text_clip(g, cname, ty, p->name,
                      p->stopped ? GC_DIM : GC_TEXT, ccpu - cname - 12);

        /* A bar behind the number: a column of digits is a table, a column of
         * bars is a diagnosis. */
        int pct = cpu_pct[idx];
        int barw = (cstate - ccpu - 20) * pct / 100;
        if (barw > 0)
            gui_fill(g, ccpu, y + 3, barw, rh - 6,
                     gui_mix(GC_PANEL,
                             pct > 60 ? 0xE05A3A : (pct > 20 ? 0xE0B040 : 0x46C06A),
                             70));
        snprintf(t, sizeof t, "%d%%", pct);
        gui_text(g, ccpu, ty, t, GC_TEXT);

        gui_text(g, cstate, ty, state_name(p->state, p->stopped),
                 p->stopped ? GC_WARN : GC_DIM);
        if (p->pane) gui_text(g, cwin, ty, "yes", GC_DIM);
    }
    if (need_sb) gui_scrollbar(g, lx + lw - SBW, ly, SBW, lh, top, vis, nproc);

    /* status: memory, and a bar for it */
    gui_vgrad(g, 0, g->h - STAT_H, g->w, STAT_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, g->h - STAT_H, g->w, 1, GC_EDGE);
    int sty = g->h - STAT_H + (STAT_H - g->fh) / 2;

    unsigned long used = 0, total = 0;
    if (mem.total_frames) {
        used = (mem.total_frames - mem.free_frames) * mem.page_size / (1024 * 1024);
        total = mem.total_frames * mem.page_size / (1024 * 1024);
    }
    char t[160];
    snprintf(t, sizeof t, "%d processes   memory %lu / %lu MB", nproc, used, total);
    gui_text_clip(g, 10, sty, t, GC_TEXT, g->w - 240);

    int bx = g->w - 190, bw = 160, bh = g->fh + 2;
    int by = g->h - STAT_H + (STAT_H - bh) / 2;
    gui_panel(g, bx, by, bw, bh, GC_PANEL, GC_EDGE);
    if (total)
        gui_fill(g, bx + 1, by + 1, (int)((unsigned long)(bw - 2) * used / total),
                 bh - 2, used * 100 / total > 85 ? GC_WARN : GC_ACCENT);

    if (status[0]) gui_text_clip(g, 10, sty, status, GC_TEXT, g->w - 240);
}

/* --- actions -------------------------------------------------------------- */

static void act(int which) {
    if (which < 3 && sel < 0) return;
    int pid = sel >= 0 ? procs[sel].pid : 0;
    const char *name = sel >= 0 ? procs[sel].name : "";
    switch (which) {
        case 0:
            if (kill(pid) == 0) snprintf(status, sizeof status, "ended %s (pid %d)", name, pid);
            else snprintf(status, sizeof status, "cannot end pid %d", pid);
            break;
        case 1:
            if (proc_stop(pid) == 0) snprintf(status, sizeof status, "suspended %s", name);
            else snprintf(status, sizeof status, "cannot suspend pid %d", pid);
            break;
        case 2:
            if (proc_cont(pid) == 0) snprintf(status, sizeof status, "resumed %s", name);
            else snprintf(status, sizeof status, "cannot resume pid %d", pid);
            break;
        default:
            status[0] = 0;
            break;
    }
    refresh();
}

int main(void) {
    gui_t g;
    if (!gui_open(&g)) return 1;

    prev_ms = uptime_ms();
    refresh();

    int running = 1, dirty = 1, dragging_sb = 0;
    int hover_btn = -2;
    unsigned last_refresh = uptime_ms();

    while (running) {
        gui_event_t e;
        while (gui_poll(&g, &e)) {
            if (e.type == GE_KEY) {
                dirty = 1;
                key_event_t *k = &e.k;
                if (k->code == XKEY_RESIZE) { gui_sync(&g); continue; }
                switch (k->code) {
                    case XKEY_UP:   if (sel > 0) sel--; break;
                    case XKEY_DOWN: if (sel < nproc - 1) sel++; break;
                    case XKEY_HOME: sel = nproc ? 0 : -1; break;
                    case XKEY_END:  sel = nproc - 1; break;
                    case XKEY_DEL:  act(0); break;
                    case XKEY_ESC:  running = 0; break;
                    default:
                        if (k->code == XKEY_F(5)) refresh();
                        else if (k->code == XKEY_CHAR && k->ascii == 'q') running = 0;
                        break;
                }
                int vis = visible_rows(&g);
                if (sel >= 0) {
                    if (sel < top) top = sel;
                    if (sel >= top + vis) top = sel - vis + 1;
                }
                continue;
            }

            mouse_event_t *m = &e.m;
            int lx, ly, lw, lh;
            list_rect(&g, &lx, &ly, &lw, &lh);
            int rh = row_h(&g), vis = visible_rows(&g);

            int was_hover = hover;
            hover = -1;
            if (gui_in(m->x, m->y, lx, ly, lw - SBW, lh)) {
                int i = top + (m->y - ly) / rh;
                if (i >= 0 && i < nproc) hover = i;
            }
            {
                int hb = -1;
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) hb = i;
                }
                if (hb != hover_btn) { hover_btn = hb; dirty = 1; }
            }
            if (hover != was_hover || gui_acts(&e) || dragging_sb) dirty = 1;
            if (m->released & MB_LEFT) dragging_sb = 0;
            if (m->wheel) {
                top -= m->wheel * 3;
                if (top > nproc - vis) top = nproc - vis;
                if (top < 0) top = 0;
            }
            if (dragging_sb && (m->buttons & MB_LEFT))
                top = gui_scrollbar_pick(ly, lh, m->y, vis, nproc);

            if (m->pressed & MB_LEFT) {
                for (int i = 0; i < NBTN; i++) {
                    int x, y, w, h;
                    btn_rect(&g, i, &x, &y, &w, &h);
                    if (gui_in(m->x, m->y, x, y, w, h)) act(i);
                }
                if (nproc > vis && gui_in(m->x, m->y, lx + lw - SBW, ly, SBW, lh)) {
                    dragging_sb = 1;
                    top = gui_scrollbar_pick(ly, lh, m->y, vis, nproc);
                } else if (hover >= 0) {
                    sel = hover;
                    status[0] = 0;
                }
            }
        }

        /* Sample on a fixed cadence: the CPU column is a rate, so an
         * irregular interval would make it jitter for no reason. */
        unsigned now = uptime_ms();
        if (now - last_refresh > 1000) {
            refresh();
            last_refresh = now;
            dirty = 1;
        }

        if (dirty) {
            if (gui_sync(&g)) {
                draw(&g);
                gui_present(&g);
                dirty = 0;
            } else if (gui_lost(&g)) {
                break;          /* the window really is gone */
            }
        }
        sleep_ms(30);
    }

    gui_close(&g);
    return 0;
}
