#ifndef FDIALOG_H
#define FDIALOG_H

/* The Open and Save windows, for any program: the file manager, asked.
 *
 *   char path[1024];
 *   if (file_dialog(FD_SAVE, "Сохранить как", "/home/Documents", "Заметка.txt",
 *                   "txt", "Текст", path, sizeof path)) save_to(path);
 *
 * The question goes to /bin/files in a file (`--pick=FILE`); the answer comes
 * back in FILE.out -- the path, or nothing for Cancel. Until then this call
 * waits. A file manager closed with the window's own button leaves no answer
 * at all, which is Cancel too: the wait notices the process is gone. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define FD_OPEN   0
#define FD_SAVE   1
#define FD_FOLDER 2

/* 1 and the path in out[0..max), or 0 for Cancel. `dir` is where to start
 * (NULL: Documents), `name` the name to suggest for Save, `exts` the
 * extensions to show and to add ("txt,md" -- NULL or "" for every file),
 * `label` what to call them ("Текст"). */
static inline int file_dialog(int mode, const char *title, const char *dir, const char *name,
                              const char *exts, const char *label, char *out, int max) {
    static unsigned seq;
    char req[96], ans[104];
    snprintf(req, sizeof req, "/tmp/.pick-%u-%u", uptime_ms(), ++seq);
    snprintf(ans, sizeof ans, "%s.out", req);
    unlink(ans);
    int fd = open(req, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    char body[2048];
    int n = snprintf(body, sizeof body, "%s\n%s\n%s\n%s\n%s\n%s\n",
                     mode == FD_SAVE ? "save" : mode == FD_FOLDER ? "folder" : "open",
                     title ? title : "", dir ? dir : "", name ? name : "",
                     exts ? exts : "", label ? label : "");
    write(fd, body, (size_t)n);
    close(fd);

    char arg[128];
    snprintf(arg, sizeof arg, "--pick=%s", req);
    int pid = spawn_window("/bin/files", arg);
    if (pid < 0) { unlink(req); return 0; }

    int got = 0;
    for (;;) {
        sleep_ms(50);
        int a = open(ans, O_RDONLY);
        if (a >= 0) {
            /* the answer is written whole before the file manager ends */
            sleep_ms(20);
            int k = (int)read(a, out, (size_t)(max - 1));
            close(a);
            if (k < 0) k = 0;
            out[k] = 0;
            got = k > 0;
            break;
        }
        if (!proc_alive(pid)) {
            a = open(ans, O_RDONLY);
            if (a >= 0) {
                int k = (int)read(a, out, (size_t)(max - 1));
                close(a);
                if (k < 0) k = 0;
                out[k] = 0;
                got = k > 0;
            }
            break;
        }
    }
    unlink(ans);
    unlink(req);
    return got;
}

#endif
