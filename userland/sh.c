/* xyuOS Neo shell -- a userland program.
 *
 * It owns a WM pane and nothing else: the character grid, the tiling tree and
 * the keyboard belong to the kernel compositor, and this reaches its own pane
 * through the tty calls. Crashing it costs one pane, not the machine.
 *
 * The language is a small POSIX-like one: commands, pipes, redirections,
 * ; && || &, if/then/elif/else/fi, while/until/do/done, for/in/do/done,
 * variables (exported ones reach the programs started), $? $# $1.. $@,
 * * and ? in names, quotes. A line is read, cut into tokens, parsed into a
 * tree, and the tree run; a script is the same with more lines. A line that
 * opens an `if` and does not close it asks for more ("> ").
 *
 * Line editing, history (kept in ~/.sh_history) and Tab completion live
 * here rather than in the kernel, which is why the pane API is deliberately
 * dumb -- put a character somewhere, move the cursor, pick a color. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <tty.h>
#include <time.h>
#include "xyuos_syscall.h"

#define LINE_MAX 1024
#define HIST_MAX 200
#define PATH_MAX 1024
#define HIST_FILE "/home/.sh_history"

static int en;
#define T(ru, eng) (en ? (eng) : (ru))

static char cwd[PATH_MAX] = "/";
static char prev_cwd[PATH_MAX] = "/";
static char line[LINE_MAX + 1];
static int  line_len, line_cur;
static int  sh_row, sh_col;          /* where the input line starts on screen */
static const char *prompt_more;      /* "> " while a block is being typed */

static char hist[HIST_MAX][LINE_MAX + 1];
static int  hist_count, hist_pos;
static int  last_status;
static int  interactive = 1;

/* --- paths --------------------------------------------------------------- */

static const char *home(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : "/home";
}

/* Resolve `in` against the working directory into `out`, collapsing "." and
 * ".." so the kernel only ever sees clean absolute paths. "~" is the home
 * folder. */
static void resolve(const char *in, char *out) {
    char tmp[PATH_MAX];
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/')) snprintf(tmp, sizeof tmp, "%s%s", home(), in + 1);
    else if (in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else if (strcmp(cwd, "/") == 0) snprintf(tmp, sizeof(tmp), "/%s", in);
    else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);

    int stack[128], depth = 0, o = 0;
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
        if (depth < 128 && o + len + 1 < PATH_MAX) {
            stack[depth++] = o;
            if (o > 1) out[o++] = '/';
            for (int k = 0; k < len; k++) out[o++] = tmp[start + k];
        }
    }
    out[o > 1 ? o : 1] = '\0';
    if (o <= 1) { out[0] = '/'; out[1] = '\0'; }
}

static int is_dir(const char *p) {
    struct xyuos_stat st;
    return xyuos_stat(p, &st) == 0 && st.is_dir;
}

/* --- variables ------------------------------------------------------------- */
/* The shell's own, and the environment: `export` moves one into the
 * environment, which every program started afterwards inherits. */

#define MAX_VARS 128
#define VAR_NAME_MAX 64
#define VAR_VAL_MAX 1024
static struct { char name[VAR_NAME_MAX]; char val[VAR_VAL_MAX]; } vars[MAX_VARS];
static int var_count;

/* $0..$9, $#, $@ -- a script's own arguments */
static char *pos_args[10];
static int pos_count;

static const char *var_get(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0) return vars[i].val;
    const char *e = getenv(name);
    return e ? e : "";
}

static void var_set(const char *name, const char *val) {
    if (getenv(name)) { setenv(name, val, 1); return; }    /* exported stays exported */
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

static void var_unset(const char *name) {
    unsetenv(name);
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0) { vars[i] = vars[--var_count]; return; }
}

static void var_export(const char *name, const char *val) {
    const char *v = val;
    char keep[VAR_VAL_MAX];
    if (!v) {
        for (int i = 0; i < var_count; i++) if (strcmp(vars[i].name, name) == 0) {
            snprintf(keep, sizeof keep, "%s", vars[i].val);
            v = keep;
        }
        if (!v) v = getenv(name) ? getenv(name) : "";
    } else {
        snprintf(keep, sizeof keep, "%s", val);
        v = keep;
    }
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0) { vars[i] = vars[--var_count]; break; }
    setenv(name, v, 1);
}

static int name_char(char c, int first) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
}

/* --- the tokens -------------------------------------------------------------- */

#define TK_WORD 0
#define TK_SEMI 1      /* ;  */
#define TK_AND  2      /* && */
#define TK_OR   3      /* || */
#define TK_PIPE 4      /* |  */
#define TK_AMP  5      /* &  */
#define TK_NL   6      /* end of a line */
#define TK_END  7

#define MAX_TOKS 2048
static struct tok { int t; char *s; } toks[MAX_TOKS];
static int ntoks, tpos;
static char tokstore[16384];
static int tokused;

/* Cut `in` into words and operators. A word keeps its quotes (they are taken
 * off when it is expanded, so a quoted ">" is not a redirection). Returns 0
 * if a quote is still open -- the input goes on on the next line. */
static int lex(const char *in) {
    ntoks = 0;
    tokused = 0;
    const char *p = in;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (ntoks >= MAX_TOKS - 2) break;
        struct tok *k = &toks[ntoks];
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (*p == '\n') { k->t = TK_NL; k->s = 0; ntoks++; p++; continue; }
        if (*p == ';') { k->t = TK_SEMI; k->s = 0; ntoks++; p++; continue; }
        if (p[0] == '&' && p[1] == '&') { k->t = TK_AND; k->s = 0; ntoks++; p += 2; continue; }
        if (p[0] == '|' && p[1] == '|') { k->t = TK_OR; k->s = 0; ntoks++; p += 2; continue; }
        if (*p == '|') { k->t = TK_PIPE; k->s = 0; ntoks++; p++; continue; }
        if (*p == '&') { k->t = TK_AMP; k->s = 0; ntoks++; p++; continue; }
        /* a redirection is a word of its own: ">", ">>", "<", "2>", "2>>" */
        const char *start = p;
        if ((p[0] == '>' || p[0] == '<') || (p[0] == '2' && p[1] == '>')) {
            if (p[0] == '2') p++;
            if (p[0] == '>' && p[1] == '>') p += 2; else p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ';' && *p != '|' &&
                   *p != '&' && *p != '<' && *p != '>' && *p != '\r') {
                if (*p == '\'' || *p == '"') {
                    char q = *p++;
                    while (*p && *p != q) { if (q == '"' && *p == '\\' && p[1]) p++; p++; }
                    if (!*p) return 0;
                    p++;
                } else if (*p == '\\' && p[1]) p += 2;
                else if (p[0] == '$' && p[1] == '(' && p[2] == '(') {
                    int depth = 0;
                    p += 3;
                    while (*p) {
                        if (*p == '(') depth++;
                        else if (*p == ')') { if (!depth && p[1] == ')') { p += 2; break; } if (depth) depth--; }
                        p++;
                    }
                } else p++;
            }
        }
        int n = (int)(p - start);
        if (tokused + n + 1 > (int)sizeof tokstore) break;
        k->t = TK_WORD;
        k->s = tokstore + tokused;
        memcpy(k->s, start, (size_t)n);
        k->s[n] = 0;
        tokused += n + 1;
        ntoks++;
    }
    toks[ntoks].t = TK_END;
    toks[ntoks].s = 0;
    return 1;
}

/* --- the tree ---------------------------------------------------------------------- */

#define N_CMD   0
#define N_PIPE  1
#define N_AND   2
#define N_OR    3
#define N_SEQ   4
#define N_IF    5
#define N_WHILE 6
#define N_UNTIL 7
#define N_FOR   8
#define N_GROUP 9

typedef struct node {
    int kind, bg;
    char **w;  int nw;              /* N_CMD */
    struct node **k; int nk;        /* N_PIPE */
    struct node *a, *b, *c;         /* AND OR SEQ: a b; IF: a cond, b then, c else; loops: a cond/words, b body */
    char *var;                      /* N_FOR */
} node;

static char arena[262144];
static int arena_used;
static void *aalloc(int n) {
    n = (n + 7) & ~7;
    if (arena_used + n > (int)sizeof arena) return 0;
    void *p = arena + arena_used;
    arena_used += n;
    memset(p, 0, (size_t)n);
    return p;
}

static int parse_error;      /* 1: wrong; 2: not finished yet -- more lines needed */

static int at(int t) { return toks[tpos].t == t; }
static int at_word(const char *w) { return toks[tpos].t == TK_WORD && strcmp(toks[tpos].s, w) == 0; }
static void skip_nl(void) { while (at(TK_NL) || at(TK_SEMI)) tpos++; }
static int is_stop(void) {
    if (!at(TK_WORD)) return 0;
    static const char *stops[] = { "then", "elif", "else", "fi", "do", "done", "}", 0 };
    for (int i = 0; stops[i]; i++) if (strcmp(toks[tpos].s, stops[i]) == 0) return 1;
    return 0;
}
static void expect(const char *w) {
    skip_nl();
    if (at_word(w)) { tpos++; return; }
    parse_error = at(TK_END) ? 2 : 1;
}

static node *parse_list(void);

static node *parse_command(void) {
    if (parse_error) return 0;
    if (at(TK_END)) { parse_error = 2; return 0; }
    if (at_word("if")) {
        tpos++;
        node *n = aalloc(sizeof(node));
        n->kind = N_IF;
        n->a = parse_list();
        expect("then");
        n->b = parse_list();
        skip_nl();
        if (at_word("elif")) {
            /* elif is an if inside the else */
            toks[tpos].s = "if";
            n->c = parse_command();
            return n;
        }
        if (at_word("else")) { tpos++; n->c = parse_list(); }
        expect("fi");
        return n;
    }
    if (at_word("while") || at_word("until")) {
        node *n = aalloc(sizeof(node));
        n->kind = at_word("while") ? N_WHILE : N_UNTIL;
        tpos++;
        n->a = parse_list();
        expect("do");
        n->b = parse_list();
        expect("done");
        return n;
    }
    if (at_word("for")) {
        tpos++;
        node *n = aalloc(sizeof(node));
        n->kind = N_FOR;
        if (!at(TK_WORD)) { parse_error = at(TK_END) ? 2 : 1; return 0; }
        n->var = toks[tpos++].s;
        node *ws = aalloc(sizeof(node));
        ws->kind = N_CMD;
        ws->w = aalloc(sizeof(char *) * 256);
        if (at_word("in")) {
            tpos++;
            while (at(TK_WORD) && ws->nw < 256) ws->w[ws->nw++] = toks[tpos++].s;
        } else {
            /* no list: the script's arguments */
            for (int i = 1; i < pos_count && ws->nw < 256; i++) ws->w[ws->nw++] = pos_args[i];
        }
        n->a = ws;
        expect("do");
        n->b = parse_list();
        expect("done");
        return n;
    }
    if (at_word("{")) {
        tpos++;
        node *n = aalloc(sizeof(node));
        n->kind = N_GROUP;
        n->a = parse_list();
        expect("}");
        return n;
    }
    node *n = aalloc(sizeof(node));
    n->kind = N_CMD;
    n->w = aalloc(sizeof(char *) * 64);
    while (at(TK_WORD) && n->nw < 64) {
        if (n->nw == 0 && is_stop()) break;
        n->w[n->nw++] = toks[tpos++].s;
    }
    if (!n->nw) { parse_error = at(TK_END) ? 2 : 1; return 0; }
    return n;
}

static node *parse_pipeline(void) {
    node *first = parse_command();
    if (!first || !at(TK_PIPE)) return first;
    node *p = aalloc(sizeof(node));
    p->kind = N_PIPE;
    p->k = aalloc(sizeof(node *) * 16);
    p->k[p->nk++] = first;
    while (at(TK_PIPE) && p->nk < 16) {
        tpos++;
        while (at(TK_NL)) tpos++;
        node *c = parse_command();
        if (!c) return 0;
        p->k[p->nk++] = c;
    }
    return p;
}

static node *parse_and_or(void) {
    node *l = parse_pipeline();
    while (l && (at(TK_AND) || at(TK_OR))) {
        int kind = at(TK_AND) ? N_AND : N_OR;
        tpos++;
        while (at(TK_NL)) tpos++;
        node *r = parse_pipeline();
        if (!r) return 0;
        node *n = aalloc(sizeof(node));
        n->kind = kind;
        n->a = l;
        n->b = r;
        l = n;
    }
    return l;
}

static node *parse_list(void) {
    node *head = 0;
    skip_nl();
    while (!parse_error && !at(TK_END) && !is_stop()) {
        node *n = parse_and_or();
        if (!n) break;
        if (at(TK_AMP)) { n->bg = 1; tpos++; }
        else if (at(TK_SEMI) || at(TK_NL)) tpos++;
        if (!head) head = n;
        else {
            node *s = aalloc(sizeof(node));
            s->kind = N_SEQ;
            s->a = head;
            s->b = n;
            head = s;
        }
        skip_nl();
    }
    return head;
}

/* --- expansion ------------------------------------------------------------------------ */

/* $(( ... )): whole numbers, + - * / %, comparisons, parentheses, variables
 * by name with or without $. */
static const char *ar_p;
static long ar_expr(void);
static void ar_ws(void) { while (*ar_p == ' ' || *ar_p == '\t') ar_p++; }
static long ar_atom(void) {
    ar_ws();
    if (*ar_p == '(') { ar_p++; long v = ar_expr(); ar_ws(); if (*ar_p == ')') ar_p++; return v; }
    if (*ar_p == '-') { ar_p++; return -ar_atom(); }
    if (*ar_p == '+') { ar_p++; return ar_atom(); }
    if (*ar_p == '!') { ar_p++; return !ar_atom(); }
    if (*ar_p >= '0' && *ar_p <= '9') {
        long v = 0;
        while (*ar_p >= '0' && *ar_p <= '9') v = v * 10 + (*ar_p++ - '0');
        return v;
    }
    if (*ar_p == '$') ar_p++;
    char name[VAR_NAME_MAX];
    int n = 0;
    while (name_char(*ar_p, n == 0) && n < VAR_NAME_MAX - 1) name[n++] = *ar_p++;
    name[n] = 0;
    if (!n) { if (*ar_p) ar_p++; return 0; }
    return atol(var_get(name));
}
static long ar_mul(void) {
    long v = ar_atom();
    for (;;) {
        ar_ws();
        char op = *ar_p;
        if (op != '*' && op != '/' && op != '%') return v;
        ar_p++;
        long r = ar_atom();
        if (op == '*') v *= r;
        else if (r == 0) v = 0;
        else v = op == '/' ? v / r : v % r;
    }
}
static long ar_add(void) {
    long v = ar_mul();
    for (;;) {
        ar_ws();
        char op = *ar_p;
        if (op != '+' && op != '-') return v;
        ar_p++;
        long r = ar_mul();
        v = op == '+' ? v + r : v - r;
    }
}
static long ar_cmp(void) {
    long v = ar_add();
    for (;;) {
        ar_ws();
        if (ar_p[0] == '<' && ar_p[1] == '=') { ar_p += 2; v = v <= ar_add(); }
        else if (ar_p[0] == '>' && ar_p[1] == '=') { ar_p += 2; v = v >= ar_add(); }
        else if (ar_p[0] == '=' && ar_p[1] == '=') { ar_p += 2; v = v == ar_add(); }
        else if (ar_p[0] == '!' && ar_p[1] == '=') { ar_p += 2; v = v != ar_add(); }
        else if (ar_p[0] == '<') { ar_p++; v = v < ar_add(); }
        else if (ar_p[0] == '>') { ar_p++; v = v > ar_add(); }
        else return v;
    }
}
static long ar_expr(void) {
    long v = ar_cmp();
    for (;;) {
        ar_ws();
        if (ar_p[0] == '&' && ar_p[1] == '&') { ar_p += 2; long r = ar_cmp(); v = v && r; }
        else if (ar_p[0] == '|' && ar_p[1] == '|') { ar_p += 2; long r = ar_cmp(); v = v || r; }
        else return v;
    }
}

/* A word with its variables put in and its quotes taken off. `quoted` is
 * set if any of it was quoted (a quoted * is not a pattern). */
static void expand_word(const char *in, char *out, int outsize, int *globbable) {
    int o = 0, glob = 0;
    int dq = 0;
    for (int i = 0; in[i] && o < outsize - 1; ) {
        char c = in[i];
        if (c == '\'' && !dq) {
            i++;
            while (in[i] && in[i] != '\'' && o < outsize - 1) out[o++] = in[i++];
            if (in[i] == '\'') i++;
            continue;
        }
        if (c == '"') { dq = !dq; i++; continue; }
        if (c == '\\' && in[i + 1]) {
            char n = in[i + 1];
            if (!dq || n == '"' || n == '\\' || n == '$') { out[o++] = n; i += 2; continue; }
            out[o++] = c; i++;
            continue;
        }
        if (c == '$' && in[i + 1] == '(' && in[i + 2] == '(') {
            /* $(( arithmetic )) up to its matching )) */
            int depth = 0, j = i + 3;
            for (; in[j]; j++) {
                if (in[j] == '(') depth++;
                else if (in[j] == ')') { if (!depth && in[j + 1] == ')') break; if (depth) depth--; }
            }
            char ex[256];
            int el = j - (i + 3);
            if (el > (int)sizeof ex - 1) el = (int)sizeof ex - 1;
            memcpy(ex, in + i + 3, (size_t)el);
            ex[el] = 0;
            ar_p = ex;
            char num[24];
            snprintf(num, sizeof num, "%ld", ar_expr());
            for (int k = 0; num[k] && o < outsize - 1; k++) out[o++] = num[k];
            i = in[j] ? j + 2 : j;
            continue;
        }
        if (c == '$' && in[i + 1]) {
            i++;
            char name[VAR_NAME_MAX];
            char num[24];
            const char *v = 0;
            if (in[i] == '?') { snprintf(num, sizeof num, "%d", last_status); v = num; i++; }
            else if (in[i] == '#') { snprintf(num, sizeof num, "%d", pos_count > 0 ? pos_count - 1 : 0); v = num; i++; }
            else if (in[i] == '$') { snprintf(num, sizeof num, "%u", (unsigned)uptime_ms()); v = num; i++; }
            else if (in[i] >= '0' && in[i] <= '9') {
                int d = in[i++] - '0';
                v = d < pos_count && pos_args[d] ? pos_args[d] : "";
            } else if (in[i] == '@' || in[i] == '*') {
                static char all[VAR_VAL_MAX];
                int a = 0;
                all[0] = 0;
                for (int k = 1; k < pos_count; k++)
                    a += snprintf(all + a, sizeof all - (size_t)a, "%s%s", k > 1 ? " " : "", pos_args[k]);
                v = all;
                i++;
            } else if (in[i] == '{') {
                int n = 0;
                i++;
                while (in[i] && in[i] != '}' && n < VAR_NAME_MAX - 1) name[n++] = in[i++];
                name[n] = 0;
                if (in[i] == '}') i++;
                v = var_get(name);
            } else if (name_char(in[i], 1)) {
                int n = 0;
                while (in[i] && name_char(in[i], 0) && n < VAR_NAME_MAX - 1) name[n++] = in[i++];
                name[n] = 0;
                v = var_get(name);
            } else { out[o++] = '$'; continue; }
            for (int k = 0; v && v[k] && o < outsize - 1; k++) out[o++] = v[k];
            continue;
        }
        if (c == '~' && i == 0 && (in[1] == 0 || in[1] == '/') && !dq) {
            const char *h = home();
            for (int k = 0; h[k] && o < outsize - 1; k++) out[o++] = h[k];
            i++;
            continue;
        }
        if ((c == '*' || c == '?') && !dq) glob = 1;
        out[o++] = c;
        i++;
    }
    out[o] = 0;
    if (globbable) *globbable = glob;
}

/* `*` any run, `?` one character (a whole UTF-8 one). */
static int glob_match(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (;;) {
            if (glob_match(pat + 1, s)) return 1;
            if (!*s) return 0;
            s++;
            while (((unsigned char)*s & 0xC0) == 0x80) s++;
        }
    }
    if (!*s) return 0;
    if (*pat == '?') {
        s++;
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
        return glob_match(pat + 1, s);
    }
    return *pat == *s && glob_match(pat + 1, s + 1);
}

/* The argv a command is run with, its words expanded: variables, then
 * patterns in the last part of a path. */
#define MAX_ARGS 256
static char argstore[65536];
static int argused;
static char *arg_dup(const char *s) {
    int n = (int)strlen(s);
    if (argused + n + 1 > (int)sizeof argstore) return (char *)"";
    char *d = argstore + argused;
    memcpy(d, s, (size_t)n + 1);
    argused += n + 1;
    return d;
}

static int expand_glob(const char *pat, char **out, int max) {
    char dir[PATH_MAX], abs[PATH_MAX];
    const char *slash = strrchr(pat, '/');
    const char *leaf = slash ? slash + 1 : pat;
    if (slash) {
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - pat), pat);
        if (!dir[0]) strcpy(dir, "/");
    } else strcpy(dir, ".");
    if (strchr(dir, '*') || strchr(dir, '?')) return 0;
    resolve(dir, abs);
    static struct xdirent ents[512];
    int n = readdir_x(abs, ents, 512);
    if (n > 512) n = 512;
    int got = 0;
    /* in order, as ls would show them */
    int idx[512];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && strcmp(ents[idx[j]].name, ents[v].name) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    for (int k = 0; k < n && got < max; k++) {
        const char *nm = ents[idx[k]].name;
        if (nm[0] == '.' && leaf[0] != '.') continue;
        if (!glob_match(leaf, nm)) continue;
        char full[PATH_MAX];
        if (slash) snprintf(full, sizeof full, "%.*s/%s", (int)(slash - pat), pat, nm);
        else snprintf(full, sizeof full, "%s", nm);
        out[got++] = arg_dup(full);
    }
    return got;
}

/* --- builtins --------------------------------------------------------------------------- */

static void help(const char *topic);

static int cmd_cd(const char *arg) {
    char path[PATH_MAX];
    if (arg && strcmp(arg, "-") == 0) { resolve(prev_cwd, path); printf("%s\n", path); }
    else resolve(arg && *arg ? arg : home(), path);
    struct xyuos_stat st;
    if (xyuos_stat(path, &st) != 0) { printf(T("cd: нет такой папки: %s\n", "cd: no such directory: %s\n"), path); return 1; }
    if (!st.is_dir) { printf(T("cd: это не папка: %s\n", "cd: not a directory: %s\n"), path); return 1; }
    snprintf(prev_cwd, sizeof prev_cwd, "%s", cwd);
    snprintf(cwd, sizeof(cwd), "%s", path);
    setenv("PWD", cwd, 1);
    return 0;
}

/* test / [ : the usual conditions on files, strings and numbers. */
static int cmd_test(int argc, char **argv) {
    if (argc > 0 && strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) { printf("[: %s\n", T("нет «]»", "missing ]")); return 2; }
        argc--;
    }
    argv++; argc--;
    int neg = 0;
    if (argc > 0 && strcmp(argv[0], "!") == 0) { neg = 1; argv++; argc--; }
    int r = 1;
    if (argc == 0) r = 1;
    else if (argc == 1) r = argv[0][0] ? 0 : 1;
    else if (argc == 2) {
        const char *op = argv[0];
        char p[PATH_MAX];
        resolve(argv[1], p);
        struct xyuos_stat st;
        int ex = xyuos_stat(p, &st) == 0;
        if (!strcmp(op, "-e")) r = !ex;
        else if (!strcmp(op, "-f")) r = !(ex && !st.is_dir);
        else if (!strcmp(op, "-d")) r = !(ex && st.is_dir);
        else if (!strcmp(op, "-s")) r = !(ex && st.size > 0);
        else if (!strcmp(op, "-z")) r = argv[1][0] != 0;
        else if (!strcmp(op, "-n")) r = argv[1][0] == 0;
        else r = 2;
    } else if (argc == 3) {
        const char *a = argv[0], *op = argv[1], *b = argv[2];
        long x = atol(a), y = atol(b);
        if (!strcmp(op, "=") || !strcmp(op, "==")) r = strcmp(a, b) != 0;
        else if (!strcmp(op, "!=")) r = strcmp(a, b) == 0;
        else if (!strcmp(op, "-eq")) r = !(x == y);
        else if (!strcmp(op, "-ne")) r = !(x != y);
        else if (!strcmp(op, "-lt")) r = !(x < y);
        else if (!strcmp(op, "-le")) r = !(x <= y);
        else if (!strcmp(op, "-gt")) r = !(x > y);
        else if (!strcmp(op, "-ge")) r = !(x >= y);
        else r = 2;
    } else r = 2;
    if (r == 2) { printf("test: %s\n", T("не понимаю условие", "bad condition")); return 2; }
    return neg ? !r : r;
}

/* Background jobs: what is running, so the prompt can say when it ended. */
#define MAX_JOBS 16
static struct { int used, n; int pid[8]; char cmd[80]; } jobs[MAX_JOBS];

static void jobs_reap(int report) {
    for (int j = 0; j < MAX_JOBS; j++) {
        if (!jobs[j].used) continue;
        int alive = 0;
        for (int k = 0; k < jobs[j].n; k++) if (jobs[j].pid[k] > 0 && proc_alive(jobs[j].pid[k])) alive = 1;
        if (alive) continue;
        int code = 0;
        for (int k = 0; k < jobs[j].n; k++) if (jobs[j].pid[k] > 0) code = waitpid(jobs[j].pid[k]);
        if (report) printf(T("[%d] Готово (%d)  %s\n", "[%d] Done (%d)  %s\n"), j + 1, code, jobs[j].cmd);
        jobs[j].used = 0;
    }
}

static int job_add(int *pids, int n, const char *what) {
    for (int j = 0; j < MAX_JOBS; j++) {
        if (jobs[j].used) continue;
        jobs[j].used = 1;
        jobs[j].n = n > 8 ? 8 : n;
        for (int k = 0; k < jobs[j].n; k++) jobs[j].pid[k] = pids[k];
        snprintf(jobs[j].cmd, sizeof jobs[j].cmd, "%s", what);
        printf("[%d] %d\n", j + 1, pids[n - 1]);
        return j;
    }
    return -1;
}

static int run_script_file(const char *path, int argc, char **argv);

/* Remove a file or, with -r, a folder and everything in it. */
static int rm_tree(const char *p, int recursive) {
    struct xyuos_stat st;
    if (xyuos_stat(p, &st) != 0) { printf(T("rm: нет такого: %s\n", "rm: no such file: %s\n"), p); return 1; }
    if (st.is_dir && recursive) {
        static struct xdirent ents[256];
        int n = readdir_x(p, ents, 256);
        if (n > 256) n = 256;
        char (*names)[256] = malloc((size_t)(n > 0 ? n : 1) * 256);
        if (!names) return 1;
        for (int i = 0; i < n; i++) snprintf(names[i], 256, "%s", ents[i].name);
        int bad = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(names[i], ".") || !strcmp(names[i], "..")) continue;
            char c[PATH_MAX];
            snprintf(c, sizeof c, "%s/%s", strcmp(p, "/") ? p : "", names[i]);
            bad |= rm_tree(c, 1);
        }
        free(names);
        if (bad) return 1;
    }
    if (xyuos_unlink(p) != 0) {
        printf(st.is_dir && !recursive ? T("rm: %s — папка (rm -r удалит её с содержимым)\n", "rm: %s is a folder (rm -r)\n")
                                       : T("rm: не удалось удалить %s\n", "rm: cannot remove %s\n"), p);
        return 1;
    }
    return 0;
}

static int mkdir_p(const char *p) {
    char cur[PATH_MAX];
    int o = 0;
    for (int i = 0; p[i]; i++) {
        cur[o++] = p[i];
        cur[o] = 0;
        if ((p[i + 1] == '/' || p[i + 1] == 0) && o > 1 && !is_dir(cur) && xyuos_mkdir(cur) != 0) return 1;
    }
    return 0;
}

/* Builtins that answer here, in the shell: what changes the shell itself, and
 * the handful that were always here. -1 if `argv[0]` is not one. */
static int loop_ctl;                /* 1 break, 2 continue */
static int want_exit, exit_code;

/* date: the time, and the clock's settings.
 *   date               local time, with the zone
 *   date -u            UTC
 *   date sync [SERVER] set the clock from the network now
 *   date zone [NAME]   the zone, or change it
 *   date zones         the zones there are */
static void fmt_offset(char *out, int n, int off) {
    int a = off < 0 ? -off : off;
    if (a % 3600) snprintf(out, n, "UTC%c%d:%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
    else snprintf(out, n, a ? "UTC%c%d" : "UTC", off < 0 ? '-' : '+', a / 3600);
}

static int cmd_date(int argc, char **argv) {
    static const char *wd_ru[7] = { "вс", "пн", "вт", "ср", "чт", "пт", "сб" };
    static const char *wd_en[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct clock_info ci;
    sysclock_info(&ci);
    char off[24];

    if (argc >= 2 && !strcmp(argv[1], "sync")) {
        printf(T("спрашиваю время у %s...\n", "asking %s for the time...\n"),
               argc >= 3 ? argv[2] : "pool.ntp.org");
        if (!net_up()) { printf(T("date: нет сети\n", "date: no network\n")); return 1; }
        int r = sysclock_sync(argc >= 3 ? argv[2] : 0);
        if (r < 0) { printf(T("date: нет сети\n", "date: no network\n")); return 1; }
        if (r == 0) { printf(T("date: сервер времени не ответил\n", "date: no time server answered\n")); return 1; }
        sysclock_info(&ci);
        int d = ci.delta_ms, ad = d < 0 ? -d : d;
        printf(T("часы установлены по %s: поправка %c%d.%03d с, ответ за %d мс\n",
                 "clock set from %s: off by %c%d.%03d s, answer in %d ms\n"),
               ci.server, d < 0 ? '-' : '+', ad / 1000, ad % 1000, ci.rtt_ms);
        argc = 1;              /* and show it */
    }
    if (argc >= 2 && !strcmp(argv[1], "zones")) {
        struct clock_zone z;
        for (int i = 0; sysclock_zone(i, &z) == 0; i++) {
            fmt_offset(off, sizeof off, z.offset_now);
            printf("%s%-20s %-9s %s\n", strcmp(z.name, ci.zone) ? "  " : "* ",
                   z.name, off, en ? z.en : z.ru);
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "zone")) {
        if (argc >= 3) {
            /* A city's own name finds its zone too: "date zone Moscow". */
            const char *want = argv[2];
            struct clock_zone z;
            const char *hit = 0;
            static char found[40];
            for (int i = 0; sysclock_zone(i, &z) == 0 && !hit; i++) {
                const char *slash = strchr(z.name, '/');
                /* ...and so does a Russian one, any of the cities it lists. */
                int ru_hit = 0;
                size_t wl = strlen(want);
                for (const char *c = z.ru; *c && !ru_hit; ) {
                    if (!strncmp(c, want, wl) && (c[wl] == ',' || c[wl] == 0)) ru_hit = 1;
                    while (*c && *c != ',') c++;
                    while (*c == ',' || *c == ' ') c++;
                }
                if (ru_hit || !strcasecmp(z.name, want) || (slash && !strcasecmp(slash + 1, want))) {
                    snprintf(found, sizeof found, "%s", z.name);
                    hit = found;
                }
            }
            if (!hit || sysclock_set_zone(hit) != 0) {
                printf(T("date: нет такого пояса: %s (список: date zones)\n",
                         "date: no such zone: %s (see: date zones)\n"), want);
                return 1;
            }
            sysclock_info(&ci);
        }
        fmt_offset(off, sizeof off, ci.offset);
        printf("%s (%s%s)\n", ci.zone, off, ci.dst ? T(", летнее время", ", summer time") : "");
        return 0;
    }

    int utc = argc >= 2 && !strcmp(argv[1], "-u");
    if (argc >= 2 && !utc) {
        printf(T("использование: date [-u] | date sync [сервер] | date zone [пояс] | date zones\n",
                 "usage: date [-u] | date sync [server] | date zone [name] | date zones\n"));
        return 2;
    }
    time_t now = (time_t)(ci.utc_ms / 1000);
    struct tm *tm = utc ? gmtime(&now) : localtime(&now);
    fmt_offset(off, sizeof off, utc ? 0 : ci.offset);
    printf("%s %02d.%02d.%04d %02d:%02d:%02d %s",
           en ? wd_en[tm->tm_wday] : wd_ru[tm->tm_wday], tm->tm_mday, tm->tm_mon + 1,
           tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec, off);
    if (!utc) printf("  %s", ci.zone);
    printf("\n");
    if (!ci.synced)
        printf(T("(часы ещё не сверены с сетью: date sync)\n", "(not yet set from the network: date sync)\n"));
    return 0;
}

static int builtin(int argc, char **argv) {
    const char *cmd = argv[0];
    const char *a1 = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "exit")) { want_exit = 1; exit_code = argc > 1 ? atoi(a1) : last_status; return exit_code; }
    if (!strcmp(cmd, "cd")) return cmd_cd(argc > 1 ? a1 : 0);
    if (!strcmp(cmd, "pwd")) { printf("%s\n", cwd); return 0; }
    if (!strcmp(cmd, "true") || !strcmp(cmd, ":")) return 0;
    if (!strcmp(cmd, "false")) return 1;
    if (!strcmp(cmd, "test") || !strcmp(cmd, "[")) return cmd_test(argc, argv);
    if (!strcmp(cmd, "break")) { loop_ctl = 1; return 0; }
    if (!strcmp(cmd, "continue")) { loop_ctl = 2; return 0; }
    if (!strcmp(cmd, "help")) { help(a1); return 0; }
    if (!strcmp(cmd, "clear")) { tty_clear(); return 0; }
    if (!strcmp(cmd, "echo")) {
        int i = 1, nl = 1;
        if (argc > 1 && !strcmp(argv[1], "-n")) { nl = 0; i = 2; }
        for (; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
        if (nl) printf("\n");
        return 0;
    }
    if (!strcmp(cmd, "export")) {
        if (argc == 1) { for (char **e = environ; e && *e; e++) printf("export %s\n", *e); return 0; }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; var_export(argv[i], eq + 1); *eq = '='; }
            else var_export(argv[i], 0);
        }
        return 0;
    }
    if (!strcmp(cmd, "unset")) { for (int i = 1; i < argc; i++) var_unset(argv[i]); return 0; }
    if (!strcmp(cmd, "env")) { for (char **e = environ; e && *e; e++) printf("%s\n", *e); return 0; }
    if (!strcmp(cmd, "set")) {
        for (int i = 0; i < var_count; i++) printf("%s=%s\n", vars[i].name, vars[i].val);
        for (char **e = environ; e && *e; e++) printf("%s\n", *e);
        return 0;
    }
    if (!strcmp(cmd, "read")) {
        /* read NAME...: a line from the keyboard (or what was redirected in) */
        char buf[VAR_VAL_MAX];
        int n = 0;
        for (;;) {
            struct key_event ev;
            tty_read_key(&ev);
            if (ev.code == KEY_ENTER) break;
            if (ev.code == KEY_BKSP) {
                if (n > 0) { n--; while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--; printf("\b \b"); }
                continue;
            }
            if (ev.code == KEY_CHAR && (unsigned char)ev.ascii >= 32 && n < (int)sizeof buf - 1) {
                buf[n++] = ev.ascii;
                putchar(ev.ascii);
            }
        }
        printf("\n");
        buf[n] = 0;
        /* words to the names in turn, the rest of the line to the last */
        char *p = buf;
        for (int i = 1; i < argc; i++) {
            while (*p == ' ') p++;
            char *e = p;
            if (i < argc - 1) { while (*e && *e != ' ') e++; if (*e) *e++ = 0; }
            else e = p + strlen(p);
            var_set(argv[i], p);
            p = e;
        }
        if (argc == 1) var_set("REPLY", buf);
        return 0;
    }
    if (!strcmp(cmd, "source") || !strcmp(cmd, ".")) {
        if (argc < 2) return 1;
        char p[PATH_MAX];
        resolve(a1, p);
        return run_script_file(p, argc - 1, argv + 1);
    }
    if (!strcmp(cmd, "jobs")) {
        jobs_reap(1);
        for (int j = 0; j < MAX_JOBS; j++)
            if (jobs[j].used) printf(T("[%d] Работает  %s\n", "[%d] Running  %s\n"), j + 1, jobs[j].cmd);
        return 0;
    }
    if (!strcmp(cmd, "wait")) {
        for (int j = 0; j < MAX_JOBS; j++)
            if (jobs[j].used) for (int k = 0; k < jobs[j].n; k++) if (jobs[j].pid[k] > 0) waitpid(jobs[j].pid[k]);
        jobs_reap(1);
        return 0;
    }
    if (!strcmp(cmd, "history")) {
        for (int i = 0; i < hist_count; i++) printf("%4d  %s\n", i + 1, hist[i]);
        return 0;
    }
    if (!strcmp(cmd, "rm")) {
        int r = 0, rec = 0, any = 0;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1]) { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) rec = 1; continue; }
            char p[PATH_MAX];
            resolve(argv[i], p);
            r |= rm_tree(p, rec);
            any = 1;
        }
        if (!any) { printf(T("использование: rm [-r] ФАЙЛ...\n", "usage: rm [-r] FILE...\n")); return 1; }
        return r;
    }
    if (!strcmp(cmd, "mkdir")) {
        int r = 0, par = 0, any = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-p")) { par = 1; continue; }
            char p[PATH_MAX];
            resolve(argv[i], p);
            any = 1;
            if (par) { if (mkdir_p(p)) { printf(T("mkdir: не удалось: %s\n", "mkdir: failed: %s\n"), p); r = 1; } }
            else if (xyuos_mkdir(p) != 0) { printf(T("mkdir: не удалось: %s\n", "mkdir: failed: %s\n"), p); r = 1; }
        }
        if (!any) { printf(T("использование: mkdir [-p] ПАПКА...\n", "usage: mkdir [-p] DIR...\n")); return 1; }
        return r;
    }
    if (!strcmp(cmd, "reboot")) { printf(T("перезагрузка...\n", "rebooting...\n")); xyuos_power(XYUOS_REBOOT); return 0; }
    if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "shutdown")) {
        printf(T("выключение...\n", "powering off...\n")); xyuos_power(XYUOS_OFF);
        printf(T("выключение на этой машине не поддерживается\n", "power off not supported on this machine\n"));
        return 1;
    }
    if (!strcmp(cmd, "suspend")) { xyuos_power(XYUOS_SLEEP); return 0; }
    if (!strcmp(cmd, "date")) return cmd_date(argc, argv);
    if (!strcmp(cmd, "ifconfig") || !strcmp(cmd, "ip")) {
        if (!net_up()) { printf(T("сеть: нет связи (есть ли сетевая карта или телефон-модем?)\n",
                                  "net: no link (is there a NIC or a tethered phone?)\n")); return 1; }
        struct net_info ni;
        if (net_config(&ni) != 0) { printf(T("сеть: выключена\n", "net: down\n")); return 1; }
        printf("ip   %u.%u.%u.%u\n", (ni.ip>>24)&0xFF,(ni.ip>>16)&0xFF,(ni.ip>>8)&0xFF,ni.ip&0xFF);
        printf("gw   %u.%u.%u.%u\n", (ni.gw>>24)&0xFF,(ni.gw>>16)&0xFF,(ni.gw>>8)&0xFF,ni.gw&0xFF);
        printf("mask %u.%u.%u.%u\n", (ni.mask>>24)&0xFF,(ni.mask>>16)&0xFF,(ni.mask>>8)&0xFF,ni.mask&0xFF);
        printf("dns  %u.%u.%u.%u\n", (ni.dns>>24)&0xFF,(ni.dns>>16)&0xFF,(ni.dns>>8)&0xFF,ni.dns&0xFF);
        return 0;
    }
    if (!strcmp(cmd, "smp")) {
        unsigned int N = 1000000u;
        if (argc >= 2) { N = (unsigned)atol(a1); if (N < 2) N = 1000000u; }
        struct smp_result r;
        printf(T("считаю простые числа до %u на одном ядре, потом на всех...\n",
                 "counting primes below %u on 1 core, then all cores...\n"), N);
        if (smp_bench((unsigned long long)N, &r) != 0) { printf("smp: failed\n"); return 1; }
        printf(T("ядер        %d\n", "cores        %d\n"), r.cores);
        printf(T("1 ядро      %u мс\n", "1 core       %u ms\n"), r.ms_single);
        printf(T("%d ядер     %u мс\n", "%d cores     %u ms\n"), r.cores, r.ms_multi);
        if (r.ms_multi > 0) {
            unsigned int s10 = r.ms_single * 10u / r.ms_multi;
            printf(T("ускорение   %u.%ux\n", "speedup      %u.%ux\n"), s10 / 10u, s10 % 10u);
        }
        printf(T("простых     %u\n", "primes       %u\n"), (unsigned int)r.primes);
        return 0;
    }
    if (!strcmp(cmd, "ping")) {
        if (argc < 2) { printf(T("использование: ping АДРЕС\n", "usage: ping HOST\n")); return 1; }
        if (!net_up()) { printf(T("ping: нет сети\n", "ping: no network\n")); return 1; }
        unsigned int ip = 0;
        if (net_resolve(a1, &ip) != 0) { printf(T("ping: не найден адрес «%s»\n", "ping: cannot resolve '%s'\n"), a1); return 1; }
        printf("PING %s (%u.%u.%u.%u)\n", a1, (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF);
        int okc = 0;
        for (int i = 0; i < 4; i++) {
            int rtt = net_ping(a1);
            if (rtt >= 0) {
                if (rtt >= 1000) printf(T("  ответ %d  время=%u.%u мс\n", "  reply seq=%d  time=%u.%u ms\n"),
                                        i, (unsigned)rtt / 1000u, ((unsigned)rtt % 1000u) / 100u);
                else printf(T("  ответ %d  время=%u мкс\n", "  reply seq=%d  time=%u us\n"), i, (unsigned)rtt);
                okc++;
            } else printf(T("  %d  нет ответа\n", "  seq=%d  timeout\n"), i);
            sleep_ms(300);
        }
        printf(T("получено %d из 4\n", "%d/4 received\n"), okc);
        return okc ? 0 : 1;
    }
    if (!strcmp(cmd, "wget") || !strcmp(cmd, "curl")) {
        if (argc < 2) { printf(T("использование: wget АДРЕС [файл]\n", "usage: wget URL [outfile]\n")); return 1; }
        const char *u = a1;
        int tls = 0;
        if (strncmp(u, "http://", 7) == 0) u += 7;
        else if (strncmp(u, "https://", 8) == 0) { u += 8; tls = 1; }
        char host[128], path[256];
        int port = tls ? 443 : 80, hi = 0;
        while (*u && *u != '/' && *u != ':' && hi < (int)sizeof(host)-1) host[hi++] = *u++;
        host[hi] = 0;
        if (*u == ':') { u++; port = 0; while (*u >= '0' && *u <= '9') port = port*10 + (*u++ - '0'); }
        if (*u == '/') { int pi = 0; while (*u && pi < (int)sizeof(path)-1) path[pi++] = *u++; path[pi] = 0; }
        else { path[0] = '/'; path[1] = 0; }
        static char body[65536];
        printf(T("соединяюсь с %s:%d ...\n", "connecting to %s:%d ...\n"), host, port);
        int nb = tls ? net_https_get(host, path, port, body, sizeof(body))
                     : net_http_get(host, path, port, body, sizeof(body));
        if (nb < 0) { printf(T("wget: запрос к %s не удался\n", "wget: request to %s failed\n"), host); return 1; }
        if (argc > 2) {
            char p[PATH_MAX]; resolve(argv[2], p);
            FILE *f = fopen(p, "w");
            if (!f) { printf(T("wget: не создать «%s»\n", "wget: cannot create '%s'\n"), p); return 1; }
            fwrite(body, 1, nb, f); fclose(f);
            printf(T("сохранено %d байт -> %s\n", "saved %d bytes -> %s\n"), nb, p);
        } else {
            fwrite(body, 1, nb, stdout);
            printf(T("\n[%d байт]\n", "\n[%d bytes]\n"), nb);
        }
        return 0;
    }
    return -1;
}

/* --- running ------------------------------------------------------------------------------ */

/* A command's argv and redirections, from its words. */
struct cmdline {
    char *argv[MAX_ARGS];
    int argc;
    char in[PATH_MAX], out[PATH_MAX];
    int append;
    int nassign;                         /* NAME=VALUE words before the command */
    char *assign[16];
};

static int build_cmd(node *n, struct cmdline *c) {
    c->argc = 0;
    c->in[0] = c->out[0] = 0;
    c->append = 0;
    c->nassign = 0;
    char buf[VAR_VAL_MAX * 2];
    for (int i = 0; i < n->nw; i++) {
        const char *w = n->w[i];
        int is_out = !strcmp(w, ">") || !strcmp(w, "2>"), is_app = !strcmp(w, ">>") || !strcmp(w, "2>>"),
            is_in = !strcmp(w, "<");
        if ((is_out || is_app || is_in) && i + 1 < n->nw) {
            expand_word(n->w[++i], buf, sizeof buf, 0);
            if (w[0] == '2') continue;               /* there is one output; errors go with it */
            if (is_in) resolve(buf, c->in);
            else { resolve(buf, c->out); c->append = is_app; }
            continue;
        }
        /* NAME=VALUE before the command */
        if (c->argc == 0 && name_char(w[0], 1)) {
            const char *eq = w;
            while (*eq && name_char(*eq, eq == w)) eq++;
            if (*eq == '=' && c->nassign < 16) {
                expand_word(w, buf, sizeof buf, 0);
                c->assign[c->nassign++] = arg_dup(buf);
                continue;
            }
        }
        int glob = 0;
        expand_word(w, buf, sizeof buf, &glob);
        if (glob) {
            int got = expand_glob(buf, c->argv + c->argc, MAX_ARGS - 1 - c->argc);
            if (got > 0) { c->argc += got; continue; }
        }
        if (c->argc < MAX_ARGS - 1) c->argv[c->argc++] = arg_dup(buf);
    }
    c->argv[c->argc] = 0;
    return c->argc;
}

static int find_prog(const char *cmd, char *path) {
    if (strchr(cmd, '/')) { resolve(cmd, path); return 1; }
    /* $PATH, as many folders as it names */
    const char *pp = getenv("PATH");
    if (!pp || !*pp) pp = "/bin";
    while (*pp) {
        const char *e = pp;
        while (*e && *e != ':') e++;
        snprintf(path, PATH_MAX, "%.*s/%s", (int)(e - pp), pp, cmd);
        struct xyuos_stat st;
        if (xyuos_stat(path, &st) == 0 && !st.is_dir) return 1;
        pp = *e ? e + 1 : e;
    }
    snprintf(path, PATH_MAX, "/bin/%s", cmd);
    return 0;
}

/* The words of a node, as text, for the job list. */
static void node_text(node *n, char *out, int max) {
    out[0] = 0;
    if (n->kind == N_CMD) {
        int o = 0;
        for (int i = 0; i < n->nw && o < max - 2; i++) o += snprintf(out + o, (size_t)(max - o), "%s%s", i ? " " : "", n->w[i]);
    } else if (n->kind == N_PIPE) {
        int o = 0;
        for (int k = 0; k < n->nk && o < max - 4; k++) {
            char part[80];
            node_text(n->k[k], part, sizeof part);
            o += snprintf(out + o, (size_t)(max - o), "%s%s", k ? " | " : "", part);
        }
    }
}

/* Has Ctrl+C been pressed while the shell itself was busy (a loop of builtins)? */
static int interrupted(void) {
    key_event_t ev;
    while (poll_event(&ev)) {
        if (ev.code == XKEY_CHAR && (ev.mods & XMOD_CTRL) && (ev.ascii == 'c' || ev.ascii == 'C')) {
            printf("^C\n");
            return 1;
        }
    }
    return 0;
}

static int exec(node *n);

static int run_cmd(node *n, int bg) {
    struct cmdline c;
    int argmark = argused;
    build_cmd(n, &c);
    if (c.argc == 0) {
        /* only assignments */
        for (int i = 0; i < c.nassign; i++) {
            char *eq = strchr(c.assign[i], '=');
            *eq = 0;
            var_set(c.assign[i], eq + 1);
            *eq = '=';
        }
        argused = argmark;
        return 0;
    }
    /* builtins run in the shell (a redirect for one sends its output too) */
    int r = -1;
    if (!bg && !c.out[0]) r = builtin(c.argc, c.argv);
    if (r >= 0) { argused = argmark; return r; }

    char path[PATH_MAX];
    find_prog(c.argv[0], path);
    /* VAR=x cmd: for that command only */
    char saved[16][VAR_VAL_MAX];
    int had[16];
    for (int i = 0; i < c.nassign; i++) {
        char *eq = strchr(c.assign[i], '=');
        *eq = 0;
        const char *old = getenv(c.assign[i]);
        had[i] = old != 0;
        if (old) snprintf(saved[i], VAR_VAL_MAX, "%s", old);
        setenv(c.assign[i], eq + 1, 1);
        *eq = '=';
    }
    int pid = spawn_full(path, c.argv, c.argc, c.in[0] ? c.in : 0, c.out[0] ? c.out : 0, c.append, -1, -1);
    for (int i = 0; i < c.nassign; i++) {
        char *eq = strchr(c.assign[i], '=');
        *eq = 0;
        if (had[i]) setenv(c.assign[i], saved[i], 1); else unsetenv(c.assign[i]);
        *eq = '=';
    }
    argused = argmark;
    if (pid < 0) {
        printf(T("%s: команда не найдена\n", "%s: command not found\n"), c.argv[0]);
        return 127;
    }
    if (bg) {
        char what[80];
        node_text(n, what, sizeof what);
        job_add(&pid, 1, what);
        return 0;
    }
    return waitpid(pid);
}

#define MAX_STAGES 16
static int run_pipe(node *n, int bg) {
    int np = n->nk - 1;
    int pfd[MAX_STAGES], pid[MAX_STAGES];
    for (int i = 0; i < n->nk; i++) pid[i] = -1;
    for (int i = 0; i < np; i++) {
        pfd[i] = pipe_new();
        if (pfd[i] < 0) {
            printf(T("pipe: нет свободных каналов\n", "pipe: out of pipes\n"));
            for (int k = 0; k < i; k++) pipe_close(pfd[k]);
            return 1;
        }
    }
    int failed = 0;
    int argmark = argused;
    for (int i = 0; i < n->nk; i++) {
        node *st = n->k[i];
        if (st->kind != N_CMD) { printf(T("в конвейере — только простые команды\n", "only simple commands in a pipe\n")); failed = 1; break; }
        struct cmdline c;
        if (!build_cmd(st, &c)) { failed = 1; break; }
        char path[PATH_MAX];
        find_prog(c.argv[0], path);
        int in_pipe = i > 0 ? pfd[i - 1] : -1;
        int out_pipe = i < n->nk - 1 ? pfd[i] : -1;
        const char *fin = (i == 0 && c.in[0]) ? c.in : 0;
        const char *fout = (i == n->nk - 1 && c.out[0]) ? c.out : 0;
        pid[i] = spawn_full(path, c.argv, c.argc, fin, fout, c.append, in_pipe, out_pipe);
        if (pid[i] < 0) { printf(T("%s: команда не найдена\n", "%s: command not found\n"), c.argv[0]); failed = 1; break; }
    }
    argused = argmark;
    if (failed) {
        for (int i = 0; i < np; i++) pipe_close(pfd[i]);
        for (int i = 0; i < n->nk; i++) if (pid[i] >= 0) kill(pid[i], SIGKILL);
    }
    if (bg && !failed) {
        char what[80];
        node_text(n, what, sizeof what);
        job_add(pid, n->nk, what);
        return 0;
    }
    int last = 0;
    for (int i = 0; i < n->nk; i++) {
        if (pid[i] < 0) continue;
        int code = waitpid(pid[i]);
        if (i == n->nk - 1) last = code;
    }
    return failed ? 127 : last;
}

static int exec(node *n) {
    if (!n || want_exit) return last_status;
    switch (n->kind) {
    case N_CMD:   last_status = run_cmd(n, n->bg); break;
    case N_PIPE:  last_status = run_pipe(n, n->bg); break;
    case N_SEQ:   exec(n->a); if (!loop_ctl && !want_exit) exec(n->b); break;
    case N_AND:   exec(n->a); if (!loop_ctl && !want_exit && last_status == 0) exec(n->b); break;
    case N_OR:    exec(n->a); if (!loop_ctl && !want_exit && last_status != 0) exec(n->b); break;
    case N_GROUP: exec(n->a); break;
    case N_IF:
        exec(n->a);
        if (loop_ctl || want_exit) break;
        if (last_status == 0) exec(n->b);
        else if (n->c) exec(n->c);
        else last_status = 0;
        break;
    case N_WHILE:
    case N_UNTIL: {
        int guard = 0;
        last_status = 0;
        for (;;) {
            exec(n->a);
            int go = n->kind == N_WHILE ? last_status == 0 : last_status != 0;
            if (!go || want_exit) break;
            exec(n->b);
            if (loop_ctl == 1) { loop_ctl = 0; break; }
            if (loop_ctl == 2) loop_ctl = 0;
            if ((++guard & 15) == 0 && interrupted()) { last_status = 130; break; }
        }
        if (last_status != 130) last_status = 0;
        break;
    }
    case N_FOR: {
        char buf[VAR_VAL_MAX * 2];
        node *ws = n->a;
        last_status = 0;
        for (int i = 0; i < ws->nw && !want_exit; i++) {
            int glob = 0;
            expand_word(ws->w[i], buf, sizeof buf, &glob);
            char *list[MAX_ARGS];
            int cnt = 0;
            int argmark = argused;
            if (glob) cnt = expand_glob(buf, list, MAX_ARGS);
            if (!cnt) { list[0] = arg_dup(buf); cnt = 1; }
            for (int k = 0; k < cnt && !want_exit; k++) {
                var_set(n->var, list[k]);
                exec(n->b);
                if (loop_ctl == 1) break;
                if (loop_ctl == 2) loop_ctl = 0;
                if (interrupted()) { last_status = 130; loop_ctl = 1; break; }
            }
            argused = argmark;
            if (loop_ctl == 1) { loop_ctl = 0; break; }
        }
        break;
    }
    }
    return last_status;
}

/* Parse and run text: a line, or a whole script. 2 if it is not finished
 * (an `if` without its `fi`), 1 if it could not be understood. */
static int run_text(const char *text) {
    if (!lex(text)) return 2;
    tpos = 0;
    arena_used = 0;
    parse_error = 0;
    node *n = parse_list();
    if (!parse_error && !at(TK_END)) parse_error = 1;
    if (parse_error == 2) return 2;
    if (parse_error) {
        printf(T("sh: не понимаю «%s»\n", "sh: syntax error near '%s'\n"),
               toks[tpos].t == TK_WORD ? toks[tpos].s : at(TK_END) ? T("конец строки", "end of line") : "?");
        last_status = 2;
        return 1;
    }
    argused = 0;
    exec(n);
    loop_ctl = 0;
    return 0;
}

static int run_script_file(const char *path, int argc, char **argv) {
    long fd = xyuos_open(path);
    if (fd < 0) { printf(T("sh: не открыть %s\n", "sh: cannot open %s\n"), path); return 1; }
    static char buf[65536];
    long n = 0, got;
    while (n < (long)sizeof buf - 1 && (got = xyuos_read(fd, buf + n, sizeof buf - 1 - (unsigned long)n)) > 0) n += got;
    xyuos_close(fd);
    buf[n] = 0;
    /* the arguments, for this script */
    char *saved[10];
    int saved_n = pos_count;
    for (int i = 0; i < 10; i++) saved[i] = pos_args[i];
    pos_count = 0;
    for (int i = 0; i < argc && i < 10; i++) pos_args[pos_count++] = argv[i];
    char *text = buf;
    if (text[0] == '#' && text[1] == '!') { while (*text && *text != '\n') text++; }
    int r = run_text(text);
    if (r == 2) { printf(T("sh: %s: скрипт оборвался посреди блока\n", "sh: %s: unexpected end of script\n"), path); last_status = 2; }
    for (int i = 0; i < 10; i++) pos_args[i] = saved[i];
    pos_count = saved_n;
    return last_status;
}

/* --- help ------------------------------------------------------------------------------------ */

static void head_line(const char *s) { tty_set_color(TC_YELLOW, TC_BLACK); printf("%s\n", s); tty_reset_color(); }

static void help(const char *topic) {
    if (topic && *topic && strcmp(topic, "all") != 0) {
        if (!strcmp(topic, "syntax") || !strcmp(topic, "язык")) {
            head_line(T("язык оболочки", "shell syntax"));
            printf(T("  ИМЯ=значение       переменная; $ИМЯ или ${ИМЯ} — её значение\n",
                     "  NAME=VALUE         a variable; $NAME or ${NAME}\n"));
            printf(T("  export ИМЯ[=знач]  в окружение — видна запущенным программам\n",
                     "  export NAME[=VAL]  into the environment, seen by programs started\n"));
            printf(T("  $? $# $1..$9 $@    код возврата, аргументы скрипта\n",
                     "  $? $# $1..$9 $@    exit status, script arguments\n"));
            printf(T("  а ; б   а && б   а || б   а | б   а &   (фоном; jobs, wait)\n",
                     "  a ; b   a && b   a || b   a | b   a &   (background; jobs, wait)\n"));
            printf(T("  > файл  >> файл  < файл   *.txt  ?   'как есть'  \"с $ИМЯ\"\n",
                     "  > file  >> file  < file   *.txt  ?   'literal'  \"with $NAME\"\n"));
            printf(T("  if усл; then ...; elif ...; else ...; fi\n", "  if cond; then ...; elif ...; else ...; fi\n"));
            printf(T("  while усл; do ...; done     until усл; do ...; done\n", "  while cond; do ...; done     until cond; do ...; done\n"));
            printf(T("  for f in *.txt; do echo $f; done     break continue\n", "  for f in *.txt; do echo $f; done     break continue\n"));
            printf(T("  [ -f файл ] [ -d папка ] [ $a = $b ] [ $n -lt 5 ]   (test)\n",
                     "  [ -f file ] [ -d dir ] [ $a = $b ] [ $n -lt 5 ]   (test)\n"));
            printf(T("  read ИМЯ   source файл   sh скрипт.sh арг...\n", "  read NAME   source file   sh script.sh args...\n"));
            return;
        }
        if (!strcmp(topic, "keys") || !strcmp(topic, "клавиши")) {
            head_line(T("клавиши", "keys"));
            printf(T("  Tab                дополнить имя команды или файла (дважды — варианты)\n",
                     "  Tab                complete a command or a file name (twice: the choices)\n"));
            printf(T("  стрелки вверх/вниз история (хранится в ~/.sh_history)\n", "  Up/Down            history (kept in ~/.sh_history)\n"));
            printf(T("  Ctrl+C             прервать программу или строку\n", "  Ctrl+C             stop a program or drop the line\n"));
            printf(T("  Ctrl+L             очистить экран\n", "  Ctrl+L             clear the screen\n"));
            printf(T("  Ctrl+A / Ctrl+E    в начало / в конец строки\n", "  Ctrl+A / Ctrl+E    start / end of the line\n"));
            printf(T("  Ctrl+U / Ctrl+K    стереть до начала / до конца\n", "  Ctrl+U / Ctrl+K    erase to the start / to the end\n"));
            printf(T("  мышью выделить     скопировать; Ctrl+Shift+V — вставить\n", "  select with mouse  copies; Ctrl+Shift+V pastes\n"));
            printf(T("  Alt+Shift          русская / английская раскладка\n", "  Alt+Shift          Russian / English layout\n"));
            return;
        }
        printf(T("нет раздела справки «%s»: syntax, keys\n", "no help topic '%s': syntax, keys\n"), topic);
        return;
    }
    tty_set_color(TC_GREEN, TC_BLACK); printf("xyuOS Neo\n"); tty_reset_color();
    printf(T("программы  files note view play web taskmgr devmgr control\n",
             "apps       files note view play web taskmgr devmgr control\n"));
    printf(T("файлы      ls cat cp mv rm mkdir touch stat hexdump\n", "files      ls cat cp mv rm mkdir touch stat hexdump\n"));
    printf(T("текст      grep wc head tail sort uniq tee echo edit\n", "text       grep wc head tail sort uniq tee echo edit\n"));
    printf(T("система    ps kill free uname date sleep smp clear cd pwd env export\n",
             "system     ps kill free uname date sleep smp clear cd pwd env export\n"));
    printf(T("время      date [-u], date sync (сверить по сети), date zone Москва, date zones\n",
             "time       date [-u], date sync (set from network), date zone Moscow, date zones\n"));
    printf(T("питание    reboot poweroff suspend\n", "power      reboot poweroff suspend\n"));
    printf(T("сеть       ifconfig ping wget curl\n", "net        ifconfig ping wget curl\n"));
    printf(T("флешки     /usb, /usb2, ... (FAT, exFAT; можно вынимать на ходу)\n",
             "drives     /usb, /usb2, ... (FAT, exFAT; plug and pull any time)\n"));
    printf(T("C          cc ФАЙЛ.c -o ПРОГ    run ФАЙЛ.c\n", "C          cc FILE.c -o PROG    run FILE.c\n"));
    printf(T("игры       doom\n", "games      doom\n"));
    printf("\n");
    tty_set_color(TC_CYAN, TC_BLACK); printf("help syntax"); tty_reset_color();
    printf(T("  — язык оболочки (if, for, while, переменные, &)\n", "  -- the language (if, for, while, variables, &)\n"));
    tty_set_color(TC_CYAN, TC_BLACK); printf("help keys"); tty_reset_color();
    printf(T("    — клавиши (Tab, история, буфер обмена)\n", "    -- keys (Tab, history, clipboard)\n"));
}

/* --- line editing ---------------------------------------------------------------------------- */

static void draw_prompt(void) {
    if (prompt_more) {
        printf("%s", prompt_more);
    } else {
        tty_set_color(TC_GREEN, TC_BLACK);
        printf("dyx");
        tty_reset_color();
        printf(":");
        tty_set_color(TC_CYAN, TC_BLACK);
        const char *h = home();
        size_t hl = strlen(h);
        if (strncmp(cwd, h, hl) == 0 && (cwd[hl] == 0 || cwd[hl] == '/')) printf("~%s", cwd + hl);
        else printf("%s", cwd);
        tty_reset_color();
        printf("$ ");
    }
    /* Ask the pane where the prompt actually ended, rather than computing it
     * from the prompt's length: the pane may have scrolled or wrapped, and
     * only it knows. This is what the editable span is measured from. */
    tty_getcur(&sh_row, &sh_col);
    line_len = line_cur = 0;
    line[0] = '\0';
}

/* The line is UTF-8: a Russian letter is two bytes and one place on the
 * screen. Positions in `line` are bytes; the cursor on the screen counts
 * characters -- the bytes that do not continue one. A line longer than the
 * pane goes on on the next row. */
static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int chars_before(int bytes) {
    int n = 0;
    for (int i = 0; i < bytes; i++) if (!is_cont(line[i])) n++;
    return n;
}
static int char_back(int at) {
    if (at <= 0) return 0;
    at--;
    while (at > 0 && is_cont(line[at])) at--;
    return at;
}
static int char_fwd(int at) {
    if (at >= line_len) return line_len;
    at++;
    while (at < line_len && is_cont(line[at])) at++;
    return at;
}

static int drawn_rows = 1;          /* rows the line took when last drawn */

static void redraw_line(void) {
    int cols, rows;
    tty_size(&cols, &rows);
    if (cols < 1) cols = 80;
    /* wipe what the line took last time */
    for (int r = 0; r < drawn_rows; r++) {
        tty_move(sh_row + r, r ? 0 : sh_col);
        tty_erase_line();
    }
    tty_move(sh_row, sh_col);
    for (int i = 0; i < line_len; i++) putchar(line[i]);
    int total = sh_col + chars_before(line_len);
    /* the pane scrolled if the line ran off its bottom: where it starts now */
    int er, ec;
    tty_getcur(&er, &ec);
    int used = total / cols;
    if (er - used < sh_row) sh_row = er - used;
    if (sh_row < 0) sh_row = 0;
    drawn_rows = used + 1;
    int pos = sh_col + chars_before(line_cur);
    tty_move(sh_row + pos / cols, pos % cols);
}

static void hist_add(const char *s) {
    if (!s[0]) return;
    if (hist_count > 0 && strcmp(hist[hist_count - 1], s) == 0) return;
    if (hist_count == HIST_MAX) {
        memmove(hist[0], hist[1], sizeof hist[0] * (HIST_MAX - 1));
        hist_count--;
    }
    snprintf(hist[hist_count], LINE_MAX + 1, "%s", s);
    hist_count++;
    /* kept for the next shell: appended, one a line */
    FILE *f = fopen(HIST_FILE, "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static void hist_load(void) {
    FILE *f = fopen(HIST_FILE, "r");
    if (!f) return;
    static char buf[LINE_MAX + 2];
    while (fgets(buf, sizeof buf, f)) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (!l) continue;
        if (hist_count == HIST_MAX) { memmove(hist[0], hist[1], sizeof hist[0] * (HIST_MAX - 1)); hist_count--; }
        snprintf(hist[hist_count++], LINE_MAX + 1, "%s", buf);
    }
    fclose(f);
}

/* --- Tab ------------------------------------------------------------------------------------- */

static const char *builtin_names[] = { "cd", "pwd", "exit", "export", "unset", "env", "set", "echo", "test",
    "true", "false", "read", "source", "jobs", "wait", "history", "help", "clear", "rm", "mkdir", "date",
    "ifconfig", "ping", "wget", "curl", "smp", "reboot", "poweroff", "suspend", "if", "for", "while", 0 };

#define MAX_CAND 256
static char cand[MAX_CAND][128];
static int ncand;
static int last_tab_line = -1;

static void cand_add(const char *s, int dir) {
    if (ncand >= MAX_CAND) return;
    for (int i = 0; i < ncand; i++) if (!strcmp(cand[i], s)) return;
    snprintf(cand[ncand], sizeof cand[0], "%s%s", s, dir ? "/" : "");
    ncand++;
}

static void complete(void) {
    /* the word the cursor ends */
    int ws = line_cur;
    while (ws > 0 && line[ws - 1] != ' ' && line[ws - 1] != '|' && line[ws - 1] != ';' && line[ws - 1] != '&' &&
           line[ws - 1] != '>' && line[ws - 1] != '<') ws--;
    char word[512];
    snprintf(word, sizeof word, "%.*s", line_cur - ws, line + ws);
    /* the first word of a command: a command name */
    int k = ws - 1;
    while (k >= 0 && line[k] == ' ') k--;
    int first = k < 0 || line[k] == '|' || line[k] == ';' || line[k] == '&';
    ncand = 0;
    char prefix[512];
    char dirpart[512];
    if (first && !strchr(word, '/')) {
        snprintf(prefix, sizeof prefix, "%s", word);
        dirpart[0] = 0;
        size_t pl = strlen(prefix);
        for (int i = 0; builtin_names[i]; i++) if (!strncmp(builtin_names[i], prefix, pl)) cand_add(builtin_names[i], 0);
        static struct xdirent ents[512];
        int n = readdir_x("/bin", ents, 512);
        if (n > 512) n = 512;
        for (int i = 0; i < n; i++) if (!ents[i].is_dir && !strncmp(ents[i].name, prefix, pl)) cand_add(ents[i].name, 0);
    } else {
        const char *sl = strrchr(word, '/');
        if (sl) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(sl - word) + 1, word); snprintf(prefix, sizeof prefix, "%s", sl + 1); }
        else { dirpart[0] = 0; snprintf(prefix, sizeof prefix, "%s", word); }
        char abs[PATH_MAX];
        resolve(dirpart[0] ? dirpart : ".", abs);
        static struct xdirent ents[512];
        int n = readdir_x(abs, ents, 512);
        if (n > 512) n = 512;
        size_t pl = strlen(prefix);
        for (int i = 0; i < n; i++) {
            const char *nm = ents[i].name;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            if (nm[0] == '.' && prefix[0] != '.') continue;
            if (!strncmp(nm, prefix, pl)) cand_add(nm, ents[i].is_dir != 0);
        }
    }
    if (!ncand) return;
    /* what they all share */
    char common[128];
    snprintf(common, sizeof common, "%s", cand[0]);
    for (int i = 1; i < ncand; i++) {
        int j = 0;
        while (common[j] && common[j] == cand[i][j]) j++;
        common[j] = 0;
    }
    /* never cut a letter in half */
    int cl = (int)strlen(common);
    while (cl > 0 && is_cont(common[cl])) cl--;
    common[cl] = 0;
    size_t have = strlen(prefix);
    if (strlen(common) > have || ncand == 1) {
        char add[160];
        snprintf(add, sizeof add, "%s", common + have);
        if (ncand == 1 && common[strlen(common) - 1] != '/') strcat(add, " ");
        int al = (int)strlen(add);
        if (line_len + al <= LINE_MAX) {
            memmove(line + line_cur + al, line + line_cur, (size_t)(line_len - line_cur + 1));
            memcpy(line + line_cur, add, (size_t)al);
            line_len += al;
            line_cur += al;
            line[line_len] = 0;
        }
        redraw_line();
        last_tab_line = -1;
        return;
    }
    /* nothing more to add: the second Tab shows them */
    if (last_tab_line == line_len) {
        printf("\n");
        int cols, rows;
        tty_size(&cols, &rows);
        int w = 0;
        for (int i = 0; i < ncand; i++) { int l = (int)strlen(cand[i]); if (l > w) w = l; }
        w += 2;
        int per = cols / (w > 0 ? w : 1);
        if (per < 1) per = 1;
        for (int i = 0; i < ncand; i++) {
            printf("%-*s", w, cand[i]);
            if ((i + 1) % per == 0 || i == ncand - 1) printf("\n");
        }
        char keep[LINE_MAX + 1];
        int kc = line_cur;
        snprintf(keep, sizeof keep, "%s", line);
        draw_prompt();
        snprintf(line, sizeof line, "%s", keep);
        line_len = (int)strlen(line);
        line_cur = kc;
        drawn_rows = 1;
        redraw_line();
        last_tab_line = -1;
        return;
    }
    last_tab_line = line_len;
}

/* --- the prompt loop -------------------------------------------------------------------------- */

static int read_line(void) {
    hist_pos = hist_count;
    drawn_rows = 1;
    for (;;) {
        struct key_event ev;
        tty_read_key(&ev);
        int ctrl = ev.mods & KMOD_CTRL;
        if (ev.code == KEY_CHAR && ctrl && (ev.ascii == 'c' || ev.ascii == 'C')) {
            printf("^C\n");
            line_len = line_cur = 0;
            line[0] = 0;
            return 0;
        }
        if (ev.code == KEY_CHAR && ctrl) {
            switch (ev.ascii | 0x20) {
            case 'l': tty_clear(); draw_prompt(); drawn_rows = 1; continue;
            case 'a': line_cur = 0; redraw_line(); continue;
            case 'e': line_cur = line_len; redraw_line(); continue;
            case 'u': memmove(line, line + line_cur, (size_t)(line_len - line_cur + 1));
                      line_len -= line_cur; line_cur = 0; redraw_line(); continue;
            case 'k': line_len = line_cur; line[line_len] = 0; redraw_line(); continue;
            case 'd': if (!line_len) { printf("\n"); snprintf(line, sizeof line, "exit"); line_len = 4; return 1; }
                      continue;
            default: continue;
            }
        }
        if (ev.code == KEY_ENTER) { printf("\n"); return 1; }
        if (ev.code == KEY_CHAR && ev.ascii == '\t') { complete(); continue; }
        last_tab_line = -1;
        if (ev.code == KEY_BKSP) {
            if (line_cur > 0) {
                int from = char_back(line_cur), n = line_cur - from;
                memmove(&line[from], &line[line_cur], (size_t)(line_len - line_cur + 1));
                line_cur = from; line_len -= n; line[line_len] = '\0';
                redraw_line();
            }
            continue;
        }
        if (ev.code == XKEY_DEL) {
            if (line_cur < line_len) {
                int to = char_fwd(line_cur);
                memmove(&line[line_cur], &line[to], (size_t)(line_len - to + 1));
                line_len -= to - line_cur;
                redraw_line();
            }
            continue;
        }
        if (ev.code == KEY_LEFT)  { if (line_cur > 0) { line_cur = char_back(line_cur); redraw_line(); } continue; }
        if (ev.code == KEY_RIGHT) { if (line_cur < line_len) { line_cur = char_fwd(line_cur); redraw_line(); } continue; }
        if (ev.code == XKEY_HOME) { line_cur = 0; redraw_line(); continue; }
        if (ev.code == XKEY_END)  { line_cur = line_len; redraw_line(); continue; }
        if (ev.code == KEY_UP) {
            if (hist_pos > 0) {
                hist_pos--;
                snprintf(line, sizeof(line), "%s", hist[hist_pos]);
                line_len = line_cur = (int)strlen(line);
                redraw_line();
            }
            continue;
        }
        if (ev.code == KEY_DOWN) {
            if (hist_pos < hist_count) {
                hist_pos++;
                if (hist_pos == hist_count) line[0] = '\0';
                else snprintf(line, sizeof(line), "%s", hist[hist_pos]);
                line_len = line_cur = (int)strlen(line);
                redraw_line();
            }
            continue;
        }
        /* Printable ASCII, or a byte of a UTF-8 letter (the Russian layout
         * sends each of its letters as two of them). */
        unsigned char ub = (unsigned char)ev.ascii;
        if (ev.code == KEY_CHAR && ((ub >= 32 && ub < 127) || ub >= 0x80)) {
            if (ev.mods & (KMOD_SUPER | KMOD_ALT)) continue;
            if (line_len < LINE_MAX) {
                memmove(&line[line_cur + 1], &line[line_cur], (size_t)(line_len - line_cur));
                line[line_cur] = ev.ascii;
                line_cur++; line_len++; line[line_len] = '\0';
                if (line_cur == line_len) {
                    putchar(ev.ascii);
                    /* ran onto a new row: from here the redraw has one more */
                    int cols, rows;
                    tty_size(&cols, &rows);
                    if (cols > 0) drawn_rows = (sh_col + chars_before(line_len)) / cols + 1;
                } else if (!is_cont(line[line_cur])) redraw_line();
            }
            continue;
        }
    }
}

int main(int argc, char **argv) {
    en = ui_lang() == 1;
    if (getenv("HOME")) {
        const char *h = home();
        if (is_dir(h)) snprintf(cwd, sizeof cwd, "%s", h);
    }
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        /* sh -c 'COMMANDS' [NAME ARGS...] */
        interactive = 0;
        pos_count = 0;
        for (int i = 3; i < argc && pos_count < 10; i++) pos_args[pos_count++] = argv[i];
        if (!pos_count) pos_args[pos_count++] = argv[0];
        static char text[LINE_MAX * 4];
        snprintf(text, sizeof text, "%s\n", argv[2]);
        if (run_text(text) == 2) { printf(T("sh: команда оборвалась посреди блока\n", "sh: unexpected end\n")); return 2; }
        return want_exit ? exit_code : last_status;
    }
    if (argc > 1) {
        /* sh SCRIPT ARGS... */
        interactive = 0;
        snprintf(cwd, sizeof cwd, "/");
        char abs[PATH_MAX];
        resolve(argv[1], abs);
        run_script_file(abs, argc - 1, argv + 1);
        return want_exit ? exit_code : last_status;
    }

    hist_load();
    tty_set_color(TC_GREEN, TC_BLACK);
    printf("xyuOS Neo shell.");
    tty_reset_color();
    printf(T(" «help» - справка, Tab - дополнение.\n", " 'help' for commands, Tab completes.\n"));

    static char block[16384];
    int blen = 0;
    for (;;) {
        jobs_reap(1);
        prompt_more = blen ? "> " : 0;
        draw_prompt();
        if (!read_line()) { blen = 0; continue; }
        if (!blen) hist_add(line);
        /* lines of one block are run together */
        int l = (int)strlen(line);
        if (blen + l + 2 >= (int)sizeof block) blen = 0;
        memcpy(block + blen, line, (size_t)l);
        blen += l;
        block[blen++] = '\n';
        block[blen] = 0;
        int r = run_text(block);
        if (r == 2) continue;                   /* more to come: "> " */
        blen = 0;
        if (want_exit) break;
    }
    printf(T("оболочка завершена\n", "shell exiting\n"));
    return exit_code;
}
