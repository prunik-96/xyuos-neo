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
#include <signal.h>
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
static struct si_cpu cpu;
static int have_cpu;

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

static const char *state_name(int st, int stopped) {
    if (stopped) return T("приостановлен", "suspended");
    switch (st) {
        case 1: return T("готов", "ready");
        case 2: return T("работает", "running");
        case 3: return T("ждёт", "blocked");
        case 4: return T("завершён", "exited");
        default: return "?";
    }
}

static void refresh(void) {
    int keep_pid = (sel >= 0 && sel < nproc) ? procs[sel].pid : -1;

    long n = xyuos_sysinfo(SI_PROCS, procs, sizeof procs);
    nproc = (n > 0) ? (int)(n / (long)sizeof(struct si_proc)) : 0;
    xyuos_sysinfo(SI_MEM, &mem, sizeof mem);
    have_cpu = xyuos_sysinfo(SI_CPU, &cpu, sizeof cpu) == (long)sizeof cpu;

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

/* The processor strip: a line of name and temperature, then one tile per
 * core, eight to a row. */
#define CORE_COLS 8
static int core_rows(void) {
    int n = have_cpu ? cpu.ncpu : 1;
    return (n + CORE_COLS - 1) / CORE_COLS;
}
static int cpu_h(gui_t *g) { return 8 + g->fh + 8 + core_rows() * (g->fh + 8) + 6; }

static void list_rect(gui_t *g, int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = TOOL_H + cpu_h(g) + HEAD_H;
    *w = g->w;
    *h = g->h - *y - STAT_H;
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

static const char *btn_ru[] = { "Завершить", "Приостановить", "Продолжить", "Обновить" };
static const char *btn_en[] = { "End task", "Suspend", "Resume", "Refresh" };
#define NBTN 4
#define btn_label (en ? btn_en : btn_ru)

static void btn_rect(gui_t *g, int i, int *x, int *y, int *w, int *h) {
    int bw = g->fw * 15, gap = 6;
    *w = bw; *h = TOOL_H - 12; *x = 8 + i * (bw + gap); *y = 6;
}

/* A load as a colour: green, yellow, red. */
static unsigned load_col(int pct) {
    return pct > 60 ? 0xE05A3A : (pct > 20 ? 0xE0B040 : 0x46C06A);
}

static void draw_cpu(gui_t *g, int y0, int h) {
    gui_fill(g, 0, y0, g->w, h, GC_WIN);
    gui_fill(g, 0, y0 + h - 1, g->w, 1, GC_EDGE);
    int ty = y0 + 8;
    char t[160];

    /* The temperature on the right, the name in what is left. */
    char temp[64];
    unsigned tcol = GC_DIM;
    if (!have_cpu || cpu.temp_mc == -1000000) {
        snprintf(temp, sizeof temp, T("температура: нет данных", "temperature: n/a"));
    } else {
        int tc = cpu.temp_mc;
        int neg = tc < 0;
        if (neg) tc = -tc;
        int o = snprintf(temp, sizeof temp, "%s%s%d.%d°C", T("температура ", "temperature "),
                         neg ? "-" : "", tc / 1000, (tc % 1000) / 100);
        /* Each die after it, when there is more than the control value. */
        for (int i = 0; i < cpu.nccd && i < 2 && o < (int)sizeof temp - 12; i++)
            if (cpu.ccd_mc[i] != -1000000)
                o += snprintf(temp + o, sizeof temp - (size_t)o, "  CCD%d %d°", i + 1, cpu.ccd_mc[i] / 1000);
        tcol = cpu.temp_mc >= 85000 ? 0xE05A3A : (cpu.temp_mc >= 70000 ? 0xE0B040 : GC_TEXT);
    }
    int tw = gui_len(temp) * g->fw;
    gui_text(g, g->w - tw - 12, ty, temp, tcol);

    /* The average clock of the cores that report one. */
    int sum = 0, nk = 0;
    if (have_cpu) for (int i = 0; i < cpu.ncpu; i++) if (cpu.mhz[i] > 0) { sum += cpu.mhz[i]; nk++; }
    const char *model = have_cpu && cpu.model[0] ? cpu.model : T("процессор", "processor");
    if (nk)
        snprintf(t, sizeof t, "%s   %d.%02d %s", model, sum / nk / 1000, sum / nk % 1000 / 10, T("ГГц", "GHz"));
    else
        snprintf(t, sizeof t, "%s", model);
    gui_text_clip(g, 12, ty, t, GC_TEXT, g->w - tw - 36);

    /* One tile per core: its load as a bar, its clock (or its load) as text. */
    int n = have_cpu ? cpu.ncpu : 1;
    int cols = n < CORE_COLS ? n : CORE_COLS;
    int tile_w = (g->w - 24 - (cols - 1) * 4) / cols, tile_h = g->fh + 4;
    int yy = ty + g->fh + 8;
    for (int i = 0; i < n; i++) {
        int x = 12 + (i % CORE_COLS) * (tile_w + 4);
        int y = yy + (i / CORE_COLS) * (g->fh + 8);
        int ld = have_cpu ? cpu.load[i] : 0;
        if (ld < 0) ld = 0;
        if (ld > 100) ld = 100;
        gui_panel(g, x, y, tile_w, tile_h, GC_PANEL, GC_EDGE);
        int bw = (tile_w - 2) * ld / 100;
        if (bw > 0) gui_fill(g, x + 1, y + 1, bw, tile_h - 2, gui_mix(GC_PANEL, load_col(ld), 70));
        int mz = have_cpu ? cpu.mhz[i] : 0;
        if (mz > 0) snprintf(t, sizeof t, "%d  %d.%02d", i, mz / 1000, mz % 1000 / 10);
        else        snprintf(t, sizeof t, "%d  %d%%", i, ld);
        gui_text_clip(g, x + 5, y + 2, t, GC_TEXT, tile_w - 8);
    }
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

    /* the processor */
    int ch = cpu_h(g);
    draw_cpu(g, TOOL_H, ch);

    /* column headings */
    int hy0 = TOOL_H + ch;
    gui_vgrad(g, 0, hy0, g->w, HEAD_H, GC_BAR, GC_BAR2);
    gui_fill(g, 0, hy0 + HEAD_H - 1, g->w, 1, GC_EDGE);
    int hy = hy0 + (HEAD_H - g->fh) / 2;
    gui_text(g, cpid, hy, "PID", GC_DIM);
    gui_text(g, cname, hy, T("Имя", "Name"), GC_DIM);
    gui_text(g, ccpu, hy, T("ЦП", "CPU"), GC_DIM);
    gui_text(g, cstate, hy, T("Состояние", "State"), GC_DIM);
    gui_text(g, cwin, hy, T("Окно", "Window"), GC_DIM);

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
                             load_col(pct),
                             70));
        snprintf(t, sizeof t, "%d%%", pct);
        gui_text(g, ccpu, ty, t, GC_TEXT);

        gui_text(g, cstate, ty, state_name(p->state, p->stopped),
                 p->stopped ? GC_WARN : GC_DIM);
        if (p->pane) gui_text(g, cwin, ty, T("да", "yes"), GC_DIM);
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
    snprintf(t, sizeof t, T("процессов: %d   память %lu / %lu МБ", "%d processes   memory %lu / %lu MB"), nproc, used, total);
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
            /* SIGKILL: End task is not a request. */
            if (kill(pid, SIGKILL) == 0) snprintf(status, sizeof status, T("завершён %s (pid %d)", "ended %s (pid %d)"), name, pid);
            else snprintf(status, sizeof status, T("не удалось завершить pid %d", "cannot end pid %d"), pid);
            break;
        case 1:
            if (proc_stop(pid) == 0) snprintf(status, sizeof status, T("приостановлен %s", "suspended %s"), name);
            else snprintf(status, sizeof status, T("не удалось приостановить pid %d", "cannot suspend pid %d"), pid);
            break;
        case 2:
            if (proc_cont(pid) == 0) snprintf(status, sizeof status, T("продолжен %s", "resumed %s"), name);
            else snprintf(status, sizeof status, T("не удалось продолжить pid %d", "cannot resume pid %d"), pid);
            break;
        default:
            status[0] = 0;
            break;
    }
    refresh();
}

int main(void) {
    gui_t g;
    en = ui_lang() == 1;
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
