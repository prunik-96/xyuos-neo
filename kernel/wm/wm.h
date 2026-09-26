#ifndef WM_H
#define WM_H

#include "pane.h"

// The window manager is the compositor: it owns the tiling tree, the pane
// grids, the framebuffer and the keyboard. Userland programs own the CONTENT
// of a pane and reach it only through the tty syscalls.
//
// It is not a loop of its own. wm_poll() is driven from the scheduler's idle
// path (see process.c), which is exactly when there is nothing better to do:
// every process is blocked, typically on input this call is about to deliver.

// Set up the tree with one fullscreen pane and start /bin/sh in it.
void wm_start(void);

// Drain the keyboard, act on WM key bindings, route the rest to the focused
// pane, and repaint if anything changed. Cheap and non-blocking when idle.
void wm_poll(void);

// Hold off every repaint (1) and allow them again (0). For the screen-off
// sleep: the screen stays black until whoever blanked it says otherwise.
void wm_set_asleep(int on);

// Repaint the whole screen right now.
void wm_refresh(void);

// The pane owned by `pid`, or NULL. This is how the tty syscalls find the
// caller's pane -- a process can only ever touch its own.
struct pane *wm_pane_for_pid(int pid);

// Take an event off `p`'s queue. Returns 0 if the queue is empty.
int wm_pane_pop_event(struct pane *p, struct kbd_event *out);

// Take a pointer event off p's queue (client-area coordinates). 0 if empty.
int wm_pane_pop_mouse(struct pane *p, struct pane_mouse *out);

// --- message boxes ---------------------------------------------------------
// Put a modal dialog on the desktop. `kind` picks the icon and the sound;
// `code` is shown verbatim so a person can repeat it back to someone.
//
// This is the counterpart of the kernel panic screen for things that are NOT
// the kernel's fault: a program that faulted, a file that would not save. The
// serial log records everything, but nobody watching the screen reads a
// serial log, and a message that only exists in a scrollback did not happen.
#define MB_ERROR 0
#define MB_WARN  1
#define MB_INFO  2

void wm_message_box(int kind, const char *code, const char *title,
                    const char *text, const char *detail);
int  wm_message_pending(void);

// Mark the screen as needing a repaint (called by the tty write syscall).
void wm_mark_dirty(void);

// Called from the keyboard IRQ: move queued events into the focused pane (or
// record a WM key binding for wm_poll to act on) and wake the owner.
void wm_route_input(void);

// Called when a process exits, so its pane can be closed or handed back.
void wm_notify_exit(int pid);

// Turn frame profiling on or off. Averages are printed to the serial log
// every 60 frames.
void wm_profile(int on);

// Appearance and pointer settings, for the control panel.
struct ui_palette;
void        wm_palette(struct ui_palette *out);
int         wm_theme_get(void);
void        wm_theme_set(int i);
int         wm_theme_count(void);
const char *wm_theme_name(int i);
int         wm_dblclick_ms(void);
void        wm_set_dblclick_ms(int ms);

// --- drag and drop ---------------------------------------------------------
// A program starts a drag; from then on the COMPOSITOR owns it, because the
// pointer is about to leave the window that started it and that window will
// stop hearing about it. The WM draws the label under the cursor, tells
// whichever window the pointer is over that something is hovering, and on
// release hands the payload to the window it was let go on.
int  wm_drag_begin(const char *payload, const char *label);
void wm_drag_cancel(void);
int  wm_drag_active(void);
// Collect what was dropped on this pane, if anything. Returns the length.
int  wm_drop_take(struct pane *p, char *out, int max);

// Open a NEW window running `path` (with an optional single argument), the
// way the start menu does. Returns the child's pid, or -1. This is how a
// program opens a document in a window of its own instead of taking over the
// one it was launched from.
int wm_spawn_window(const char *path, const char *arg);

// Called from the shell's `hyper` command to tile the current pane.
void wm_request_split(void);

// --- graphics-mode panes (SYS_GFX) ---
// Interior pixel size of the pane (inside its border).
void wm_pane_interior(struct pane *p, int *w, int *h);
// Blit an ARGB frame (w x h) into the pane and present it. 0/-1.
int  wm_pane_blit(struct pane *p, const uint32_t *src, int w, int h);
// Leave graphics mode and restore the text grid.
void wm_pane_gfx_end(struct pane *p);

#endif
