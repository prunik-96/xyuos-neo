/* xyuOS Neo file manager -- a userland program.
 *
 * Started by the shell as `fm [dir]`, it takes over the pane until 'q'.
 *
 * Note what disappeared in the move out of the kernel: the old in-kernel
 * version had to call wm_refresh() by hand around every modal prompt, because
 * its input loop was the WM's input loop and nothing else would repaint. Here
 * a prompt is just a loop that reads keys, and the compositor repaints on its
 * own -- the whole class of "typed characters stay invisible" bugs is gone. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <tty.h>
#include "upath.h"

#define FM_MAX 256
#define NAMELEN 96

static char cwd[UPATH_MAX] = "/";
static char names[FM_MAX][NAMELEN];
static int  isdir[FM_MAX];
static int  count;
static int  sel, top;

static char iobuf[8192];

static void add_entry(const char *name, int is_dir, void *ctx) {
    (void)ctx;
    if (count >= FM_MAX) return;
    if (strcmp(name, ".") == 0) return;          /* "." is noise; ".." is not */
    snprintf(names[count], NAMELEN, "%s", name);
    isdir[count] = is_dir;
    count++;
}

static void load(void) {
    count = 0;
    long n = xyuos_listdir(cwd, iobuf, sizeof(iobuf));
    if (n > 0) upath_each_entry(iobuf, n, add_entry, 0);
    if (sel >= count) sel = count > 0 ? count - 1 : 0;
}

static void draw(void) {
    int cols, rows;
    tty_size(&cols, &rows);
    load();
    tty_reset_color();
    tty_clear();

    int list_rows = (rows >= 3) ? rows - 2 : 1;
    if (sel < top) top = sel;
    if (sel >= top + list_rows) top = sel - list_rows + 1;

    tty_move(0, 0);
    tty_set_color(TC_WHITE, TC_BLUE);
    printf(" FM %s", cwd);
    tty_erase_line();
    tty_reset_color();

    for (int r = 0; r < list_rows; r++) {
        int idx = top + r;
        tty_move(1 + r, 0);
        if (idx >= count) { tty_reset_color(); tty_erase_line(); continue; }
        if (idx == sel)        tty_set_color(TC_BLACK, TC_WHITE);
        else if (isdir[idx])   tty_set_color(TC_CYAN, TC_BLACK);
        else                   tty_reset_color();
        printf("%c %s", isdir[idx] ? '/' : ' ', names[idx]);
        tty_erase_line();      /* extend the row background across the pane */
        tty_reset_color();
    }

    tty_move(rows - 1, 0);
    tty_set_color(TC_WHITE, TC_BLUE);
    printf(" Enter open  Bksp up  n new  k dir  d del  r ren  e edit  q quit ");
    tty_erase_line();
    tty_reset_color();
}

/* Modal single-line prompt on the bottom row. Returns the length typed. */
static int prompt(const char *label, char *out, int size) {
    int cols, rows;
    tty_size(&cols, &rows);

    tty_move(rows - 1, 0);
    tty_set_color(TC_WHITE, TC_BLUE);
    printf("%s", label);
    tty_erase_line();
    tty_move(rows - 1, (int)strlen(label) + 1);

    int len = 0;
    out[0] = '\0';
    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        if (ev.code == KEY_ENTER) break;
        if (ev.code == KEY_BKSP) {
            if (len > 0) { len--; out[len] = '\0'; putchar('\b'); }
        } else if (ev.code == KEY_CHAR && ev.ascii >= 32 && ev.ascii < 127 &&
                   len < size - 1) {
            if (ev.mods & (KMOD_SUPER | KMOD_ALT | KMOD_CTRL)) continue;
            out[len++] = ev.ascii;
            out[len] = '\0';
            putchar(ev.ascii);
        }
    }
    tty_reset_color();
    return len;
}

static int confirm(const char *label) {
    int cols, rows;
    tty_size(&cols, &rows);
    tty_move(rows - 1, 0);
    tty_set_color(TC_WHITE, TC_RED);
    printf("%s", label);
    tty_erase_line();
    tty_reset_color();

    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        if (ev.code != KEY_CHAR) continue;
        if (ev.ascii == 'y' || ev.ascii == 'Y') return 1;
        if (ev.ascii == 'n' || ev.ascii == 'N') return 0;
    }
}

static void view_file(const char *path) {
    int cols, rows;
    tty_size(&cols, &rows);

    tty_reset_color();
    tty_clear();
    tty_set_color(TC_YELLOW, TC_BLACK);
    printf("-- %s --\n", path);
    tty_reset_color();

    int fd = open(path, 0);
    if (fd >= 0) {
        char chunk[256];
        long got;
        while ((got = read(fd, chunk, sizeof(chunk))) > 0)
            for (long k = 0; k < got; k++) putchar(chunk[k]);
        close(fd);
    } else {
        printf("(cannot open)\n");
    }

    tty_move(rows - 1, 0);
    tty_set_color(TC_BLACK, TC_WHITE);
    printf(" [any key to return] ");
    tty_reset_color();

    struct key_event ev;
    tty_read_key(&ev);
}

static void selected_path(char *out) {
    upath_resolve(cwd, names[sel], out);
}

int main(int argc, char **argv) {
    if (argc > 1) upath_resolve("/", argv[1], cwd);

    sel = top = 0;
    draw();

    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        char path[UPATH_MAX];

        if (ev.code == KEY_CHAR && ev.ascii == 'q') break;

        if (ev.code == KEY_UP) {
            if (sel > 0) sel--;
        } else if (ev.code == KEY_DOWN) {
            if (sel < count - 1) sel++;
        } else if (ev.code == KEY_LEFT || ev.code == KEY_BKSP) {
            char up[UPATH_MAX];
            upath_resolve(cwd, "..", up);
            strcpy(cwd, up);
            sel = top = 0;
        } else if (ev.code == KEY_ENTER || ev.code == KEY_RIGHT) {
            if (count == 0) { draw(); continue; }
            selected_path(path);
            if (isdir[sel]) {
                strcpy(cwd, path);
                sel = top = 0;
            } else {
                view_file(path);
            }
        } else if (ev.code == KEY_CHAR && ev.ascii == 'n') {
            char name[NAMELEN];
            if (prompt("new file:", name, sizeof(name)) > 0) {
                upath_resolve(cwd, name, path);
                xyuos_create(path);
            }
        } else if (ev.code == KEY_CHAR && ev.ascii == 'k') {
            char name[NAMELEN];
            if (prompt("new dir:", name, sizeof(name)) > 0) {
                upath_resolve(cwd, name, path);
                xyuos_mkdir(path);
            }
        } else if (ev.code == KEY_CHAR && ev.ascii == 'd') {
            if (count > 0) {
                char label[NAMELEN + 20];
                snprintf(label, sizeof(label), " delete %s? ", names[sel]);
                if (confirm(label)) {
                    selected_path(path);
                    xyuos_unlink(path);
                    if (sel > 0) sel--;
                }
            }
        } else if (ev.code == KEY_CHAR && ev.ascii == 'r') {
            if (count > 0) {
                char name[NAMELEN];
                if (prompt("rename to:", name, sizeof(name)) > 0) {
                    char dst[UPATH_MAX];
                    selected_path(path);
                    upath_resolve(cwd, name, dst);
                    xyuos_rename(path, dst);
                }
            }
        } else if (ev.code == KEY_CHAR && ev.ascii == 'e') {
            if (count > 0 && !isdir[sel]) {
                selected_path(path);
                /* The editor is a separate program; hand it the pane by
                 * waiting for it, then redraw over whatever it left. */
                char *args[] = { "edit", path };
                int pid = spawn("/bin/edit", args, 2);
                if (pid >= 0) waitpid(pid);
            }
        }

        draw();
    }

    tty_reset_color();
    tty_clear();
    return 0;
}
