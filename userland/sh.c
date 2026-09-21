/* xyuOS Neo shell -- a userland program.
 *
 * It owns a WM pane and nothing else: the character grid, the tiling tree and
 * the keyboard belong to the kernel compositor, and this reaches its own pane
 * through the tty calls. Crashing it costs one pane, not the machine.
 *
 * Line editing and history live here rather than in the kernel, which is why
 * the pane API is deliberately dumb -- put a character somewhere, move the
 * cursor, pick a color. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <tty.h>
#include "upath.h"

#define LINE_MAX 240
#define HIST_MAX 16
#define PATH_MAX 256

static char cwd[PATH_MAX] = "/";
static char line[LINE_MAX + 1];
static int  line_len, line_cur;
static int  sh_row, sh_col;          /* where the input line starts on screen */

static char hist[HIST_MAX][LINE_MAX + 1];
static int  hist_count, hist_pos;
static int  last_status;

/* --- path handling ------------------------------------------------------- */

/* Resolve `in` against the working directory into `out`, collapsing "." and
 * ".." so the kernel only ever sees clean absolute paths. */
static void resolve(const char *in, char *out) {
    char tmp[PATH_MAX];
    if (in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else {
        if (strcmp(cwd, "/") == 0) snprintf(tmp, sizeof(tmp), "/%s", in);
        else                        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
    }

    /* Walk the components, keeping a stack of segment offsets in `out`. */
    int stack[64], depth = 0, o = 0;
    out[o++] = '/';
    for (int i = 0; tmp[i]; ) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int start = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - start;

        if (len == 1 && tmp[start] == '.') continue;
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            if (depth > 0) o = stack[--depth];
            continue;
        }
        if (depth < 64 && o + len + 1 < PATH_MAX) {
            stack[depth++] = o;
            if (o > 1) out[o++] = '/';
            for (int k = 0; k < len; k++) out[o++] = tmp[start + k];
        }
    }
    out[o > 1 ? o : 1] = '\0';
    if (o <= 1) { out[0] = '/'; out[1] = '\0'; }
}

/* --- builtins ------------------------------------------------------------ */

/* Help is deliberately paged by topic: the full text is longer than a pane,
 * and a listing that scrolls off the top is a listing nobody reads. */

static void help_apps(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("apps\n"); tty_reset_color();
    printf("  files [dir]        the graphical file manager\n");
    printf("  note [FILE]        the text editor (mouse caret, selection)\n");
    printf("  view [FILE]        pictures (PNG, JPEG, BMP) and text\n");
    printf("  taskmgr            processes: CPU, memory, end task, suspend\n");
    printf("  play [FILE.wav]    the music player\n");
    printf("These open in their own windows. Ctrl+Esc opens the start menu,\n");
    printf("where typing a few letters finds any of them (and anything else\n");
    printf("in /bin).\n");
}

static void help_files(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("files\n"); tty_reset_color();
    printf("  ls [dir]           list a directory\n");
    printf("  cat FILE...        print files (no args: read input)\n");
    printf("  cp SRC DST         copy\n");
    printf("  mv SRC DST         rename or move\n");
    printf("  rm FILE            remove            (builtin)\n");
    printf("  mkdir DIR          create a directory (builtin)\n");
    printf("  touch FILE...      create if missing\n");
    printf("  stat FILE...       size, inode, type\n");
    printf("  hexdump [FILE]     bytes in hex and ASCII\n");
    printf("  fm [dir]           file manager in the terminal\n");
    printf("  files [dir]        the graphical one, in a window\n");
}

static void help_text(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("text\n"); tty_reset_color();
    printf("  grep [-vcni] PAT [F...]  lines containing PAT (literal, not regex)\n");
    printf("  wc [FILE...]             count lines, words, bytes\n");
    printf("  head [-n N] [FILE...]    first N lines (default 10)\n");
    printf("  tail [-n N] [FILE]       last N lines\n");
    printf("  sort [-run] [FILE]       sort; -r reverse -u unique -n numeric\n");
    printf("  uniq [-cd] [FILE]        collapse adjacent duplicates\n");
    printf("  tee FILE                 copy input to a file and onward\n");
    printf("  echo ARGS                print arguments\n");
    printf("  edit FILE                text editor in the terminal\n");
    printf("  note FILE                the graphical one, in a window\n");
    printf("Each of these reads standard input when given no file, so they\n");
    printf("work on the right-hand side of a pipe.\n");
}

static void help_system(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("system\n"); tty_reset_color();
    printf("  ps                 running processes (pid, parent, state, name)\n");
    printf("  kill PID           interrupt a process (Ctrl+C does the same)\n");
    printf("  free               physical memory in use (used/total, frames)\n");
    printf("  uname              system name and version\n");
    printf("  date               current wall-clock date and time (from the RTC)\n");
    printf("  sleep SECS         wait (fractions allowed: sleep 0.5)\n");
    printf("  smp [N]            count primes below N (default 1000000) on one\n");
    printf("                     core then on all cores, and report the speedup\n");
    printf("  clear              clear the pane     (builtin)\n");
    printf("  pwd / cd [dir]     working directory  (builtin)\n");
    printf("  set                list shell variables (builtin)\n");
    printf("  exit               close this shell   (builtin)\n");
    tty_set_color(TC_YELLOW, TC_BLACK); printf("power\n"); tty_reset_color();
    printf("  reboot             restart the machine\n");
    printf("  poweroff/shutdown  power off (ACPI)\n");
    printf("  suspend            halt until a key is pressed\n");
}

static void help_net(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("network\n"); tty_reset_color();
    printf("  ifconfig / ip      show the DHCP lease: ip, gateway, mask, dns\n");
    printf("  ping HOST          4 ICMP echoes to a name or a.b.c.d, with RTT\n");
    printf("  wget URL [file]    HTTP GET; prints the body, or saves it to file\n");
    printf("  curl URL           same as wget (prints the body)\n");
    printf("\n");
    printf("The link comes up over DHCP on first use. Only plain http:// is\n");
    printf("supported -- there is no TLS, so https:// URLs will not work.\n");
}

static void help_usb(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("usb (FAT32 stick)\n"); tty_reset_color();
    printf("  usb ls [dir]       list a directory on the stick\n");
    printf("  usb cat FILE       print a file from the stick\n");
    printf("  usb get FILE [dst] copy from the stick to the local disk\n");
    printf("  usb put FILE [dst] copy from the local disk to the stick\n");
    printf("\n");
    printf("The stick is also mounted at /usb, so ordinary commands work on it:\n");
    printf("  ls /usb            cat /usb/file      cp f /usb/f\n");
    printf("  mkdir /usb/dir     rm /usb/file       mv /usb/a /usb/b\n");
    printf("  cc /usb/prog.c     echo hi > /usb/f\n");
    printf("\n");
    printf("/usb is the only PERSISTENT storage: the root filesystem lives in RAM\n");
    printf("and is reset on every boot, files under /usb are on the physical stick.\n");
    printf("Mounted automatically on first use. Names are 8.3 (short) form.\n");
    printf("rm on a directory needs it to be empty; mv cannot cross into /usb.\n");
}

static void help_games(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("games & demos\n"); tty_reset_color();
    printf("  doom               play DOOM (shareware /doom1.wad)\n");
    printf("                     arrows move, Ctrl fire, Space use, Esc menu\n");
    printf("  hello_c hello_cpp  C / C++ 'hello world'\n");
    printf("  spin loop count    busy loops (light up the CPU monitor; ^C stops)\n");
    printf("  yes                print 'y' forever -- pipe it: yes | head\n");
    printf("  bigfile fstest     filesystem demos\n");
    printf("  parent keywait     process / keyboard demos\n");
}

static void help_c(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("compiling C\n"); tty_reset_color();
    printf("  cc FILE.c -o OUT   compile to an executable\n");
    printf("  run FILE.c [args]  compile and run in one step\n");
    printf("\n");
    printf("Headers are in /include, the library in /lib/libc.a; cc links\n");
    printf("both for you. Put the result in /bin to run it by name:\n");
    printf("  cc /demo.c -o /bin/demo   then just:  demo\n");
}

static void help_syntax(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("shell syntax\n"); tty_reset_color();
    printf("  NAME=VALUE         set a variable; use it as $NAME\n");
    printf("  $?                 exit status of the last command\n");
    printf("  cmd > FILE         send output to a file (replacing it)\n");
    printf("  cmd >> FILE        append instead\n");
    printf("  cmd < FILE         read input from a file\n");
    printf("  a | b              pipe (real: both run at once, streaming)\n");
    printf("  'x'   \"x\"          quoting; single quotes do not expand $\n");
    printf("\n");
    printf("Pipe stages run concurrently through an in-kernel buffer, so\n");
    printf("`yes | head` stops once head is done. Builtins run only as a lone\n");
    printf("command, not inside a pipeline.\n");
}

static void help_keys(void) {
    tty_set_color(TC_YELLOW, TC_BLACK); printf("keys\n"); tty_reset_color();
    printf("windows (mouse: drag the title bar, drag edges to resize)\n");
    printf("  Alt+Tab            switch windows   (Alt+Shift+Tab goes back)\n");
    printf("  Alt+F4             close the window\n");
    printf("  super+Up           maximise / restore\n");
    printf("  super+Down         minimise\n");
    printf("  super+Left/Right   snap to half the screen\n");
    printf("  super+D            show the desktop\n");
    printf("  super+shift+Enter  new window\n");
    printf("  super+shift+arrows move the window\n");
    printf("  alt+shift+arrows   resize the window\n");
    printf("  shift+PgUp/PgDn    scroll this window's history\n");
    printf("workspaces\n");
    printf("  super+1..9         switch workspace (created on first visit)\n");
    printf("desktop\n");
    printf("  super+T            switch theme (Black Aero / Aero)\n");
    printf("  super+M            toggle the System Monitor\n");
    printf("  (right ctrl works as super, for hosts that eat the super key)\n");
    printf("editor\n");
    printf("  ^S save  ^Q quit  ^Z undo  ^Y redo  ^A select all\n");
    printf("  ^C copy  ^X cut   ^V paste  shift+arrows select  Tab indent\n");
    printf("file manager\n");
    printf("  Enter open  Bksp up  n new file  k new dir\n");
    printf("  d delete  r rename  e edit  q quit\n");
}

static void cmd_help(const char *topic) {
    if (topic && *topic) {
        if (strcmp(topic, "apps") == 0)   { help_apps();   return; }
        if (strcmp(topic, "files") == 0)  { help_files();  return; }
        if (strcmp(topic, "text") == 0)   { help_text();   return; }
        if (strcmp(topic, "system") == 0) { help_system(); return; }
        if (strcmp(topic, "net") == 0 || strcmp(topic, "network") == 0)
                                          { help_net();    return; }
        if (strcmp(topic, "usb") == 0)    { help_usb();    return; }
        if (strcmp(topic, "games") == 0 || strcmp(topic, "demos") == 0)
                                          { help_games();  return; }
        if (strcmp(topic, "c") == 0)      { help_c();      return; }
        if (strcmp(topic, "syntax") == 0) { help_syntax(); return; }
        if (strcmp(topic, "keys") == 0)   { help_keys();   return; }
        if (strcmp(topic, "all") == 0) {
            help_apps();   printf("\n");
            help_files();  printf("\n");
            help_text();   printf("\n");
            help_system(); printf("\n");
            help_net();    printf("\n");
            help_usb();    printf("\n");
            help_games();  printf("\n");
            help_c();      printf("\n");
            help_syntax(); printf("\n");
            help_keys();
            return;
        }
        printf("no help topic '%s'\n", topic);
        return;
    }

    tty_set_color(TC_GREEN, TC_BLACK);
    printf("xyuOS Neo\n");
    tty_reset_color();
    printf("apps    files note view taskmgr play   (Ctrl+Esc = start menu)\n");
    printf("files   ls cat cp mv rm mkdir touch stat hexdump fm\n");
    printf("text    grep wc head tail sort uniq tee echo edit\n");
    printf("system  ps kill free uname date sleep smp clear pwd cd set exit\n");
    printf("power   reboot  poweroff/shutdown  suspend\n");
    printf("net     ifconfig ping wget curl\n");
    printf("usb     usb ls [dir] | cat FILE | get FILE [dst] | put FILE [dst]\n");
    printf("games   doom  + demos: hello_c spin loop yes count bigfile\n");
    printf("C       cc run   scripts: sh FILE runs a batch of commands\n");
    printf("syntax  NAME=VALUE  $NAME  $?  >  >>  <  |  'x'  \"x\"\n");
    printf("panes   super+shift+Enter split, super+arrows focus,\n");
    printf("        super+shift+Q close, super+shift+E one pane\n");
    printf("        super+1..9 workspaces, super+T theme, super+M monitor\n");
    printf("        Ctrl+C stops a program\n");
    printf("\n");
    tty_set_color(TC_CYAN, TC_BLACK);
    printf("help TOPIC");
    tty_reset_color();
    printf("  detail: apps files text system net usb games c syntax keys all\n");
}

static void cmd_cd(const char *arg) {
    char path[PATH_MAX];
    resolve(arg && *arg ? arg : "/", path);

    struct xyuos_stat st;
    if (xyuos_stat(path, &st) != 0) {
        printf("cd: no such directory: %s\n", path);
        last_status = 1;
        return;
    }
    if (!st.is_dir) {
        printf("cd: not a directory: %s\n", path);
        last_status = 1;
        return;
    }
    snprintf(cwd, sizeof(cwd), "%s", path);
}

/* --- command dispatch ---------------------------------------------------- */

#define MAX_WORDS 32
#define MAX_VARS 32
#define VAR_NAME_MAX 32
#define VAR_VAL_MAX 128

/* --- variables ----------------------------------------------------------- */

static struct { char name[VAR_NAME_MAX]; char val[VAR_VAL_MAX]; } vars[MAX_VARS];
static int var_count;

static const char *var_get(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0) return vars[i].val;
    return "";
}

static void var_set(const char *name, const char *val) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            snprintf(vars[i].val, VAR_VAL_MAX, "%s", val);
            return;
        }
    }
    if (var_count >= MAX_VARS) return;
    snprintf(vars[var_count].name, VAR_NAME_MAX, "%s", name);
    snprintf(vars[var_count].val, VAR_VAL_MAX, "%s", val);
    var_count++;
}

/* Expand $VAR, $? and quotes from `in` into `out`. Single quotes are literal;
 * double quotes still expand. */
static void expand(const char *in, char *out, int outsize) {
    int o = 0;
    for (int i = 0; in[i] && o < outsize - 1; ) {
        char c = in[i];

        if (c == '\'') {                       /* literal run */
            i++;
            while (in[i] && in[i] != '\'' && o < outsize - 1) out[o++] = in[i++];
            if (in[i] == '\'') i++;
            continue;
        }
        if (c == '"') { i++; continue; }       /* quotes group, but expand */

        if (c == '$' && in[i + 1]) {
            i++;
            if (in[i] == '?') {                /* last exit status */
                char num[16];
                snprintf(num, sizeof(num), "%d", last_status);
                for (int k = 0; num[k] && o < outsize - 1; k++) out[o++] = num[k];
                i++;
                continue;
            }
            char name[VAR_NAME_MAX];
            int n = 0;
            while (in[i] && (in[i] == '_' ||
                             (in[i] >= 'a' && in[i] <= 'z') ||
                             (in[i] >= 'A' && in[i] <= 'Z') ||
                             (in[i] >= '0' && in[i] <= '9')) && n < VAR_NAME_MAX - 1) {
                name[n++] = in[i++];
            }
            name[n] = '\0';
            const char *v = var_get(name);
            for (int k = 0; v[k] && o < outsize - 1; k++) out[o++] = v[k];
            continue;
        }

        out[o++] = c;
        i++;
    }
    out[o] = '\0';
}

/* Split on whitespace, but keep quoted runs together. Quotes are removed by
 * expand(), which has already run, so this only has to respect the fact that
 * a quoted space must not split -- handled by expanding AFTER splitting. */
static int split_words(char *s, char *words[]) {
    int n = 0;
    while (*s && n < MAX_WORDS) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        words[n++] = s;
        if (*s == '\'' || *s == '"') {
            char q = *s++;
            while (*s && *s != q) s++;
            if (*s) s++;
        }
        while (*s && *s != ' ' && *s != '\t') {
            if (*s == '\'' || *s == '"') {
                char q = *s++;
                while (*s && *s != q) s++;
            }
            if (*s) s++;
        }
        if (*s) *s++ = '\0';
    }
    return n;
}

/* One parsed pipeline stage: argv plus any file redirects. argv points into a
 * shared static buffer, valid only until the next parse_stage -- fine because
 * a stage is spawned (which copies argv into the kernel) before the next is
 * parsed. */
struct pstage {
    char *argv[MAX_WORDS];
    int   argc;
    char  in[UPATH_MAX];
    char  out[UPATH_MAX];
    int   append;
};

static char stage_expand[MAX_WORDS][VAR_VAL_MAX * 2];

static void parse_stage(char *segment, struct pstage *st) {
    char *raw[MAX_WORDS];
    int n = split_words(segment, raw);
    st->argc = 0;
    st->in[0] = st->out[0] = '\0';
    st->append = 0;

    /* Redirection operators are recognised AFTER splitting, so a quoted '>'
     * stays an ordinary character. */
    for (int i = 0; i < n; i++) {
        int is_out = (strcmp(raw[i], ">") == 0);
        int is_app = (strcmp(raw[i], ">>") == 0);
        int is_in  = (strcmp(raw[i], "<") == 0);
        if ((is_out || is_app || is_in) && i + 1 < n) {
            char tmp[VAR_VAL_MAX * 2];
            expand(raw[++i], tmp, sizeof(tmp));
            if (is_in) upath_resolve(cwd, tmp, st->in);
            else { upath_resolve(cwd, tmp, st->out); st->append = is_app; }
            continue;
        }
        if (st->argc < MAX_WORDS) {
            expand(raw[i], stage_expand[st->argc], sizeof(stage_expand[0]));
            st->argv[st->argc] = stage_expand[st->argc];
            st->argc++;
        }
    }
}

/* Run one command with no pipe. Handles builtins (which must run inside the
 * shell) and otherwise spawns /bin/NAME and waits. Returns 1 to exit the shell. */
static int run_simple(char *segment) {
    struct pstage st;
    parse_stage(segment, &st);
    if (st.argc == 0) return 0;

    char **words = st.argv;
    int n = st.argc;
    const char *cmd = words[0];
    const char *arg1 = (n > 1) ? words[1] : "";

    /* NAME=VALUE with no command is an assignment. */
    {
        const char *eq = strchr(cmd, '=');
        if (eq && eq != cmd && n == 1) {
            char name[VAR_NAME_MAX];
            int len = (int)(eq - cmd);
            if (len > VAR_NAME_MAX - 1) len = VAR_NAME_MAX - 1;
            memcpy(name, cmd, len);
            name[len] = '\0';
            var_set(name, eq + 1);
            last_status = 0;
            return 0;
        }
    }

    last_status = 0;

    /* Builtins are only the things that MUST run inside the shell: they change
     * its own state. ls/cat/echo are real programs in /bin so redirection and
     * pipes reach them without the shell special-casing anything. */
    if (strcmp(cmd, "exit") == 0)  return 1;
    if (strcmp(cmd, "reboot") == 0)   { printf("rebooting...\n"); xyuos_power(XYUOS_REBOOT); return 0; }
    if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "shutdown") == 0) {
        printf("powering off...\n"); xyuos_power(XYUOS_OFF);
        printf("power off not supported on this machine\n"); last_status = 1; return 0;
    }
    if (strcmp(cmd, "suspend") == 0)  { xyuos_power(XYUOS_SLEEP); return 0; }
    if (strcmp(cmd, "date") == 0) {
        struct xyuos_tm t;
        xyuos_time(&t);
        static const char *mon[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
        const char *mn = (t.mon >= 1 && t.mon <= 12) ? mon[t.mon] : "???";
        printf("%s %2d %04d  %02d:%02d:%02d UTC\n",
               mn, t.day, t.year, t.hour, t.min, t.sec);
        return 0;
    }
    if (strcmp(cmd, "usb") == 0) {
        if (!usb_mount()) { printf("usb: no FAT32 stick found\n"); last_status = 1; return 0; }
        const char *sub = (n > 1) ? words[1] : "ls";
        if (strcmp(sub, "ls") == 0) {
            const char *dpath = (n > 2) ? words[2] : "/";
            static struct usb_ent ents[128];
            int cnt = usb_list(dpath, ents, 128);
            if (cnt < 0) { printf("usb: '%s' is not a directory\n", dpath); last_status = 1; return 0; }
            for (int i = 0; i < cnt; i++)
                printf("  %s%s  %u\n", ents[i].name, ents[i].is_dir ? "/" : "", ents[i].size);
        } else if (strcmp(sub, "cat") == 0 && n > 2) {
            static char fbuf[16384];
            int nb = usb_readfile(words[2], fbuf, sizeof(fbuf) - 1);
            if (nb < 0) { printf("usb: cannot read '%s'\n", words[2]); last_status = 1; return 0; }
            fbuf[nb] = 0; printf("%s", fbuf);
        } else if (strcmp(sub, "get") == 0 && n > 2) {
            static char fbuf[65536];
            int nb = usb_readfile(words[2], fbuf, sizeof(fbuf));
            if (nb < 0) { printf("usb: cannot read '%s'\n", words[2]); last_status = 1; return 0; }
            const char *bn = words[2];
            for (const char *q = words[2]; *q; q++) if (*q == '/') bn = q + 1;
            const char *dst = (n > 3) ? words[3] : bn;
            char path[PATH_MAX]; resolve(dst, path);
            FILE *f = fopen(path, "w");
            if (!f) { printf("usb: cannot create '%s'\n", path); last_status = 1; return 0; }
            fwrite(fbuf, 1, nb, f); fclose(f);
            printf("copied %d bytes -> %s\n", nb, path);
        } else if (strcmp(sub, "put") == 0 && n > 2) {
            /* Copy a local ext2 file onto the stick. */
            static char fbuf[65536];
            char path[PATH_MAX]; resolve(words[2], path);
            FILE *f = fopen(path, "r");
            if (!f) { printf("usb: cannot open '%s'\n", path); last_status = 1; return 0; }
            int nb = (int)fread(fbuf, 1, sizeof(fbuf), f);
            fclose(f);
            if (nb < 0) nb = 0;
            const char *bn = words[2];
            for (const char *q = words[2]; *q; q++) if (*q == '/') bn = q + 1;
            const char *dst = (n > 3) ? words[3] : bn;
            char sp[PATH_MAX];
            if (dst[0] == '/') snprintf(sp, sizeof(sp), "%s", dst);
            else               snprintf(sp, sizeof(sp), "/%s", dst);
            int wr = usb_writefile(sp, fbuf, nb);
            if (wr < 0) { printf("usb: cannot write '%s' (name must be 8.3, or disk full)\n", sp); last_status = 1; return 0; }
            printf("copied %d bytes -> usb:%s\n", wr, sp);
        } else {
            printf("usage: usb [ls [DIR] | cat FILE | get FILE [DST] | put FILE [DST]]\n");
        }
        return 0;
    }
    if (strcmp(cmd, "ifconfig") == 0 || strcmp(cmd, "ip") == 0) {
        if (!net_up()) { printf("net: no link (is there a NIC?)\n"); last_status = 1; return 0; }
        struct net_info ni;
        if (net_config(&ni) != 0) { printf("net: down\n"); last_status = 1; return 0; }
        printf("ip   %u.%u.%u.%u\n", (ni.ip>>24)&0xFF,(ni.ip>>16)&0xFF,(ni.ip>>8)&0xFF,ni.ip&0xFF);
        printf("gw   %u.%u.%u.%u\n", (ni.gw>>24)&0xFF,(ni.gw>>16)&0xFF,(ni.gw>>8)&0xFF,ni.gw&0xFF);
        printf("mask %u.%u.%u.%u\n", (ni.mask>>24)&0xFF,(ni.mask>>16)&0xFF,(ni.mask>>8)&0xFF,ni.mask&0xFF);
        printf("dns  %u.%u.%u.%u\n", (ni.dns>>24)&0xFF,(ni.dns>>16)&0xFF,(ni.dns>>8)&0xFF,ni.dns&0xFF);
        return 0;
    }
    if (strcmp(cmd, "smp") == 0) {
        unsigned int N = 1000000u;
        if (n >= 2) {
            N = 0;
            for (const char *p = words[1]; *p >= '0' && *p <= '9'; p++) N = N * 10 + (*p - '0');
            if (N < 2) N = 1000000u;
        }
        struct smp_result r;
        printf("counting primes below %u on 1 core, then all cores...\n", N);
        if (smp_bench((unsigned long long)N, &r) != 0) { printf("smp: failed\n"); last_status = 1; return 0; }
        printf("cores        %d\n", r.cores);
        printf("1 core       %u ms\n", r.ms_single);
        printf("%d cores     %u ms\n", r.cores, r.ms_multi);
        if (r.ms_multi > 0) {
            unsigned int s10 = r.ms_single * 10u / r.ms_multi;
            printf("speedup      %u.%ux\n", s10 / 10u, s10 % 10u);
        }
        printf("primes       %u\n", (unsigned int)r.primes);
        return 0;
    }
    if (strcmp(cmd, "ping") == 0) {
        if (n < 2) { printf("usage: ping HOST\n"); last_status = 1; return 0; }
        unsigned int ip = 0;
        if (net_resolve(words[1], &ip) != 0) { printf("ping: cannot resolve '%s'\n", words[1]); last_status = 1; return 0; }
        printf("PING %s (%u.%u.%u.%u)\n", words[1], (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
        int ok = 0;
        for (int i = 0; i < 4; i++) {
            int rtt = net_ping(words[1]);          /* microseconds */
            if (rtt >= 0) {
                if (rtt >= 1000) printf("  reply seq=%d  time=%u.%u ms\n",
                                        i, (unsigned)rtt / 1000u, ((unsigned)rtt % 1000u) / 100u);
                else             printf("  reply seq=%d  time=%u us\n", i, (unsigned)rtt);
                ok++;
            }
            else printf("  seq=%d  timeout\n", i);
            sleep_ms(300);
        }
        printf("%d/4 received\n", ok);
        if (!ok) last_status = 1;
        return 0;
    }
    if (strcmp(cmd, "wget") == 0 || strcmp(cmd, "curl") == 0) {
        if (n < 2) { printf("usage: wget URL [outfile]\n"); last_status = 1; return 0; }
        /* Parse http://host[:port]/path (bare host[/path] also accepted). */
        const char *u = words[1];
        int tls = 0;
        if (strncmp(u, "http://", 7) == 0) u += 7;
        else if (strncmp(u, "https://", 8) == 0) { u += 8; tls = 1; }
        char host[128]; char path[256]; int port = tls ? 443 : 80;
        int hi = 0;
        while (*u && *u != '/' && *u != ':' && hi < (int)sizeof(host)-1) host[hi++] = *u++;
        host[hi] = 0;
        if (*u == ':') { u++; port = 0; while (*u >= '0' && *u <= '9') port = port*10 + (*u++ - '0'); }
        if (*u == '/') { int pi = 0; while (*u && pi < (int)sizeof(path)-1) path[pi++] = *u++; path[pi] = 0; }
        else { path[0] = '/'; path[1] = 0; }

        static char body[65536];
        printf("connecting to %s:%d ...\n", host, port);
        int nb = tls ? net_https_get(host, path, port, body, sizeof(body))
                     : net_http_get(host, path, port, body, sizeof(body));
        if (nb < 0) { printf("wget: request to %s failed\n", host); last_status = 1; return 0; }
        if (n > 2) {
            char p[PATH_MAX]; resolve(words[2], p);
            FILE *f = fopen(p, "w");
            if (!f) { printf("wget: cannot create '%s'\n", p); last_status = 1; return 0; }
            fwrite(body, 1, nb, f); fclose(f);
            printf("saved %d bytes -> %s\n", nb, p);
        } else {
            fwrite(body, 1, nb, stdout);
            printf("\n[%d bytes]\n", nb);
        }
        return 0;
    }
    if (strcmp(cmd, "help") == 0)  { cmd_help(arg1); return 0; }
    if (strcmp(cmd, "clear") == 0) { tty_clear(); return 0; }
    if (strcmp(cmd, "pwd") == 0)   { printf("%s\n", cwd); return 0; }
    if (strcmp(cmd, "cd") == 0)    { cmd_cd(arg1); return 0; }
    if (strcmp(cmd, "set") == 0) {
        for (int i = 0; i < var_count; i++)
            printf("%s=%s\n", vars[i].name, vars[i].val);
        return 0;
    }
    if (strcmp(cmd, "rm") == 0) {
        char path[PATH_MAX];
        resolve(arg1, path);
        if (xyuos_unlink(path) != 0) { printf("rm: failed on '%s'\n", path); last_status = 1; }
        return 0;
    }
    if (strcmp(cmd, "mkdir") == 0) {
        char path[PATH_MAX];
        resolve(arg1, path);
        if (xyuos_mkdir(path) != 0) { printf("mkdir: failed on '%s'\n", path); last_status = 1; }
        return 0;
    }

    char path[PATH_MAX];
    if (cmd[0] == '/') snprintf(path, sizeof(path), "%s", cmd);
    else               snprintf(path, sizeof(path), "/bin/%s", cmd);

    int pid = spawn_full(path, words, n,
                         st.in[0]  ? st.in  : 0,
                         st.out[0] ? st.out : 0,
                         st.append, -1, -1);
    if (pid < 0) {
        printf("%s: command not found\n", cmd);
        last_status = 127;
        return 0;
    }
    last_status = waitpid(pid);
    return 0;
}

/* --- pipelines ------------------------------------------------------------
 *
 * Real pipes now: `a | b` spawns BOTH concurrently, a's stdout wired to b's
 * stdin through an in-kernel ring buffer that blocks the writer when full and
 * the reader when empty. Nothing is staged on disk, and b starts immediately,
 * so `yes | head` terminates (head exits -> the kernel SIGPIPE-kills yes)
 * instead of filling the disk.
 *
 * Pipeline stages are external programs; a shell builtin only runs as a lone
 * command, not inside a pipeline (it would need a subshell we do not have).
 */

#define MAX_STAGES 8

static int run_pipeline(char *stages[], int nstages) {
    int npipes = nstages - 1;
    int pfd[MAX_STAGES];
    int pid[MAX_STAGES];
    for (int i = 0; i < nstages; i++) pid[i] = -1;

    for (int i = 0; i < npipes; i++) {
        pfd[i] = pipe_new();
        if (pfd[i] < 0) {
            printf("pipe: out of pipes\n");
            for (int k = 0; k < i; k++) pipe_close(pfd[k]);
            last_status = 1;
            return 0;
        }
    }

    int failed = 0;
    for (int i = 0; i < nstages; i++) {
        struct pstage st;
        parse_stage(stages[i], &st);
        if (st.argc == 0) { failed = 1; break; }

        char path[PATH_MAX];
        const char *cmd = st.argv[0];
        if (cmd[0] == '/') snprintf(path, sizeof(path), "%s", cmd);
        else               snprintf(path, sizeof(path), "/bin/%s", cmd);

        int in_pipe  = (i > 0)            ? pfd[i - 1] : -1;
        int out_pipe = (i < nstages - 1)  ? pfd[i]     : -1;
        /* only the ends of the pipeline may carry a file redirect */
        const char *fin  = (i == 0 && st.in[0])            ? st.in  : 0;
        const char *fout = (i == nstages - 1 && st.out[0]) ? st.out : 0;

        pid[i] = spawn_full(path, st.argv, st.argc, fin, fout, st.append,
                            in_pipe, out_pipe);
        if (pid[i] < 0) {
            printf("%s: command not found\n", cmd);
            failed = 1;
            break;
        }
    }

    /* On failure, tear the whole pipeline down: closing the pipes gives every
     * running stage EOF / a broken pipe so it exits. */
    if (failed) {
        for (int i = 0; i < npipes; i++) pipe_close(pfd[i]);
        for (int i = 0; i < nstages; i++) if (pid[i] >= 0) kill(pid[i]);
    }

    int last = 0;
    for (int i = 0; i < nstages; i++) {
        if (pid[i] < 0) continue;
        int c = waitpid(pid[i]);
        if (i == nstages - 1) last = c;
    }
    last_status = failed ? 127 : last;
    return 0;
}

static int run_line(const char *input) {
    char work[LINE_MAX * 2];
    snprintf(work, sizeof(work), "%s", input);

    /* Split on unquoted '|' into stages. */
    char *stages[MAX_STAGES];
    int nstages = 0;
    char *s = work;
    stages[nstages++] = s;
    for (; *s && nstages < MAX_STAGES; s++) {
        if (*s == '\'' || *s == '"') {          /* quoted | is literal */
            char q = *s++;
            while (*s && *s != q) s++;
            if (!*s) break;
            continue;
        }
        if (*s == '|') {
            *s = '\0';
            stages[nstages++] = s + 1;
        }
    }

    if (nstages == 1) return run_simple(stages[0]);
    return run_pipeline(stages, nstages);
}

/* --- scripts --------------------------------------------------------------
 *
 * `sh file` runs the lines of `file` instead of reading the keyboard. That is
 * the whole of batch-file support: a script is not a different language, it is
 * the same shell with a different source of lines. Which is also why the file
 * manager can hand a .sh or a .bat straight to /bin/sh and have it work. */

static int run_script(const char *path) {
    long fd = xyuos_open(path);
    if (fd < 0) {
        printf("sh: cannot open %s\n", path);
        return 1;
    }

    static char buf[4096];
    char cmd[LINE_MAX + 1];
    int  clen = 0, status = 0, first = 1;
    long n;

    while ((n = xyuos_read(fd, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (clen < LINE_MAX) cmd[clen++] = c;
                continue;
            }
            cmd[clen] = '\0';
            clen = 0;

            /* Skip a leading #! line, blank lines and comments. */
            int k = 0;
            while (cmd[k] == ' ' || cmd[k] == '\t') k++;
            int skip = (cmd[k] == '\0' || cmd[k] == '#');
            if (first && cmd[0] == '#' && cmd[1] == '!') skip = 1;
            first = 0;
            if (!skip) status = run_line(cmd + k);
        }
    }
    if (clen) {                              /* a last line with no newline */
        cmd[clen] = '\0';
        int k = 0;
        while (cmd[k] == ' ' || cmd[k] == '\t') k++;
        if (cmd[k] && cmd[k] != '#') status = run_line(cmd + k);
    }
    xyuos_close(fd);
    return status;
}

/* --- line editing -------------------------------------------------------- */

static void draw_prompt(void) {
    tty_set_color(TC_GREEN, TC_BLACK);
    printf("dyx");
    tty_reset_color();
    printf(":");
    tty_set_color(TC_CYAN, TC_BLACK);
    printf("%s", cwd);
    tty_reset_color();
    printf("$ ");

    /* Ask the pane where the prompt actually ended, rather than computing it
     * from the prompt's length: the pane may have scrolled or wrapped, and
     * only it knows. This is what the editable span is measured from. */
    tty_getcur(&sh_row, &sh_col);

    line_len = line_cur = 0;
    line[0] = '\0';
}

static void redraw_line(void) {
    tty_move(sh_row, sh_col);
    tty_erase_line();
    tty_move(sh_row, sh_col);
    for (int i = 0; i < line_len; i++) putchar(line[i]);
    int cc = sh_col + line_cur;
    int cols, rows;
    tty_size(&cols, &rows);
    if (cc >= cols) cc = cols - 1;
    tty_move(sh_row, cc);
}

static void hist_add(const char *s) {
    if (!s[0]) return;
    if (hist_count > 0 && strcmp(hist[hist_count - 1], s) == 0) return;
    if (hist_count == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) strcpy(hist[i - 1], hist[i]);
        hist_count--;
    }
    snprintf(hist[hist_count], LINE_MAX + 1, "%s", s);
    hist_count++;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char abs[PATH_MAX];
        resolve(argv[1], abs);
        return run_script(abs);
    }

    tty_set_color(TC_GREEN, TC_BLACK);
    printf("xyuOS Neo shell (userland).");
    tty_reset_color();
    printf(" 'help' for commands.\n");

    for (;;) {
        draw_prompt();

        /* Read one line. */
        for (;;) {
            struct key_event ev;
            tty_read_key(&ev);

            /* Ctrl+C at the prompt cancels the current line (it does NOT reach
             * here as a kill -- the kernel only kills the foreground process,
             * and at the prompt that is this shell, which it spares). Mirrors
             * how a real shell abandons a half-typed line. */
            if (ev.code == KEY_CHAR && (ev.mods & KMOD_CTRL) &&
                (ev.ascii == 'c' || ev.ascii == 'C')) {
                printf("^C\n");
                line_len = line_cur = 0;
                line[0] = '\0';
                break;   /* redraw a fresh prompt */
            }

            if (ev.code == KEY_ENTER) {
                printf("\n");
                break;
            }
            if (ev.code == KEY_BKSP) {
                if (line_cur > 0) {
                    memmove(&line[line_cur - 1], &line[line_cur], line_len - line_cur);
                    line_cur--; line_len--; line[line_len] = '\0';
                    redraw_line();
                }
                continue;
            }
            if (ev.code == KEY_LEFT)  { if (line_cur > 0)        { line_cur--; redraw_line(); } continue; }
            if (ev.code == KEY_RIGHT) { if (line_cur < line_len) { line_cur++; redraw_line(); } continue; }
            if (ev.code == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    snprintf(line, sizeof(line), "%s", hist[hist_pos]);
                    line_len = line_cur = strlen(line);
                    redraw_line();
                }
                continue;
            }
            if (ev.code == KEY_DOWN) {
                if (hist_pos < hist_count) {
                    hist_pos++;
                    if (hist_pos == hist_count) line[0] = '\0';
                    else snprintf(line, sizeof(line), "%s", hist[hist_pos]);
                    line_len = line_cur = strlen(line);
                    redraw_line();
                }
                continue;
            }
            if (ev.code == KEY_CHAR && ev.ascii >= 32 && ev.ascii < 127) {
                if (ev.mods & (KMOD_SUPER | KMOD_ALT | KMOD_CTRL)) continue;
                if (line_len < LINE_MAX) {
                    memmove(&line[line_cur + 1], &line[line_cur], line_len - line_cur);
                    line[line_cur] = ev.ascii;
                    line_cur++; line_len++; line[line_len] = '\0';
                    /* Echo directly when appending at the end -- the common
                     * case, and it avoids a full-line repaint per keystroke. */
                    if (line_cur == line_len) putchar(ev.ascii);
                    else redraw_line();
                }
                continue;
            }
        }

        hist_add(line);
        hist_pos = hist_count;
        if (run_line(line)) break;
    }

    printf("shell exiting\n");
    return 0;
}
