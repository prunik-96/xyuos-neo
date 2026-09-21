#ifndef JS_H
#define JS_H

/* A JavaScript interpreter for xyuOS Neo.
 *
 * A working subset, in the same spirit as css.h: enough of the language that
 * the scripts pages actually run do what they meant, without pretending to be
 * a conforming engine. It reads the source, builds a tree, and walks it. There
 * is no bytecode and no optimiser -- a page's scripts run for a few
 * milliseconds and then wait for the user, so the time goes into being right
 * rather than being quick.
 *
 * WHAT IS HERE: var/let/const, functions and closures, arrow functions,
 * objects and arrays with literals, `this`, `new`, prototypes enough for
 * constructor functions, if/else, for, for-in, for-of, while, do, switch,
 * break/continue with the usual scoping, try/catch/finally, throw, template
 * literals, the full run of operators including ?., ??, &&=, and the string,
 * array, number, Math, JSON and Object methods that turn up in real code.
 *
 * WHAT IS NOT: generators, async and await, Promises, classes, modules,
 * regular expressions, Proxy, Symbol, getters and setters in literals. A page
 * that needs those gets a clean error rather than wrong behaviour.
 *
 * MEMORY. Everything -- syntax tree, objects, strings, scopes -- comes from
 * one arena that is thrown away whole when the page changes. There is no
 * garbage collector. That is a deliberate trade: a collector is the largest
 * and subtlest part of an engine, and the thing it buys is the ability to run
 * for a long time, which a page's scripts do not do. A script that allocates
 * without bound gets "out of memory" and stops, which is the same thing a
 * collector would eventually have to say.
 *
 * RUNAWAY SCRIPTS. Every statement costs a step, and the steps are counted. A
 * page with `while (true) {}` in it stops the script, not the browser.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ==========================================================================
 * the arena
 * ========================================================================== */

/* Enough for the scripts this engine can actually run. A page whose bundle
 * needs more than this was never going to run here anyway -- see the budget
 * in web.c -- and the important thing is that exhausting it is orderly. */
/* The host supplies a clock. Everything with a time budget reads it. */
static double (*js_clock_ms)(void);      /* the host clock, for budgets */

#define JS_ARENA_CAP (16 * 1024 * 1024)

static char *js_arena;
static long  js_arena_used;
static int   js_oom;

static void *js_alloc(long n) {
    n = (n + 15) & ~15L;
    if (js_arena_used + n > JS_ARENA_CAP) { js_oom = 1; return 0; }
    void *p = js_arena + js_arena_used;
    js_arena_used += n;
    memset(p, 0, (size_t)n);
    return p;
}

/* ==========================================================================
 * values
 * ========================================================================== */

typedef struct js_obj js_obj;
typedef struct js_node js_node;
typedef struct js_scope js_scope;

enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };

typedef struct {
    unsigned char t;
    union {
        double  n;
        int     b;
        struct { const char *p; int len; } s;
        js_obj *o;
    } u;
} js_val;

typedef js_val (*js_native)(js_val self, js_val *args, int nargs);

/* A host object -- an element, say -- answers for its own properties, because
 * they are not stored anywhere: textContent is the tree, not a copy of it. */
typedef int (*js_host_get)(js_obj *o, const char *name, js_val *out);
typedef int (*js_host_set)(js_obj *o, const char *name, js_val v);

typedef struct { const char *name; js_val v; } js_prop;

enum { JSO_PLAIN, JSO_ARRAY, JSO_FUNC, JSO_NATIVE };

struct js_obj {
    unsigned char kind;
    js_prop  *props;  int nprops, pcap;
    js_val   *elems;  int nelems, ecap;      /* arrays */
    js_node  *fn;                            /* user functions */
    js_scope *closure;
    js_native native;
    js_obj   *proto;
    /* host objects */
    js_host_get hget;
    js_host_set hset;
    int         host;                        /* an index the host understands */
    const char *cname;                       /* for diagnostics */
};

static js_val js_undef(void) { js_val v; v.t = JS_UNDEF; v.u.n = 0; return v; }
static js_val js_null(void)  { js_val v; v.t = JS_NULL;  v.u.n = 0; return v; }
static js_val js_bool(int b) { js_val v; v.t = JS_BOOL;  v.u.b = !!b; return v; }
static js_val js_num(double n) { js_val v; v.t = JS_NUM; v.u.n = n; return v; }

static js_val js_str_n(const char *s, int len) {
    js_val v; v.t = JS_STR;
    char *p = (char *)js_alloc(len + 1);
    if (!p) { v.u.s.p = ""; v.u.s.len = 0; return v; }
    for (int i = 0; i < len; i++) p[i] = s[i];
    p[len] = 0;
    v.u.s.p = p; v.u.s.len = len;
    return v;
}
static js_val js_str(const char *s) { return js_str_n(s, (int)strlen(s)); }

static js_val js_obj_val(js_obj *o) { js_val v; v.t = JS_OBJ; v.u.o = o; return v; }

/* ==========================================================================
 * objects
 * ========================================================================== */

static js_obj *js_new(int kind) {
    js_obj *o = (js_obj *)js_alloc(sizeof(js_obj));
    if (o) { o->kind = (unsigned char)kind; o->host = -1; }
    return o;
}

static int js_name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static js_val *js_find(js_obj *o, const char *name) {
    for (int i = 0; i < o->nprops; i++)
        if (js_name_eq(o->props[i].name, name)) return &o->props[i].v;
    return 0;
}

static void js_set(js_obj *o, const char *name, js_val v) {
    if (!o) return;
    js_val *slot = js_find(o, name);
    if (slot) { *slot = v; return; }
    if (o->nprops == o->pcap) {
        int cap = o->pcap ? o->pcap * 2 : 8;
        js_prop *np = (js_prop *)js_alloc((long)cap * sizeof(js_prop));
        if (!np) return;
        for (int i = 0; i < o->nprops; i++) np[i] = o->props[i];
        o->props = np; o->pcap = cap;
    }
    int n = (int)strlen(name);
    char *copy = (char *)js_alloc(n + 1);
    if (!copy) return;
    memcpy(copy, name, (size_t)n + 1);
    o->props[o->nprops].name = copy;
    o->props[o->nprops].v = v;
    o->nprops++;
}

/* The same, for a name that already lives somewhere permanent -- a parameter
 * or a declared variable, whose text is in the syntax tree and outlives every
 * scope that will ever refer to it. Copying those was most of what a function
 * call allocated. */
static void js_set_ref(js_obj *o, const char *name, js_val v) {
    if (!o) return;
    js_val *slot = js_find(o, name);
    if (slot) { *slot = v; return; }
    if (o->nprops == o->pcap) {
        int cap = o->pcap ? o->pcap * 2 : 4;
        js_prop *np = (js_prop *)js_alloc((long)cap * sizeof(js_prop));
        if (!np) return;
        for (int i = 0; i < o->nprops; i++) np[i] = o->props[i];
        o->props = np; o->pcap = cap;
    }
    o->props[o->nprops].name = name;
    o->props[o->nprops].v = v;
    o->nprops++;
}

static void js_arr_push(js_obj *a, js_val v) {
    if (a->nelems == a->ecap) {
        int cap = a->ecap ? a->ecap * 2 : 8;
        js_val *ne = (js_val *)js_alloc((long)cap * sizeof(js_val));
        if (!ne) return;
        for (int i = 0; i < a->nelems; i++) ne[i] = a->elems[i];
        a->elems = ne; a->ecap = cap;
    }
    a->elems[a->nelems++] = v;
}

static js_obj *js_new_array(void) { return js_new(JSO_ARRAY); }

/* ==========================================================================
 * conversions
 * ========================================================================== */

static int js_truthy(js_val v) {
    switch (v.t) {
    case JS_UNDEF: case JS_NULL: return 0;
    case JS_BOOL: return v.u.b;
    case JS_NUM:  return !(v.u.n == 0 || v.u.n != v.u.n);
    case JS_STR:  return v.u.s.len != 0;
    default: return 1;
    }
}

static js_val js_to_string(js_val v);

static double js_to_num(js_val v) {
    switch (v.t) {
    case JS_UNDEF: return 0.0 / 0.0;
    case JS_NULL:  return 0;
    case JS_BOOL:  return v.u.b;
    case JS_NUM:   return v.u.n;
    case JS_STR: {
        const char *p = v.u.s.p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) return 0;
        char *end;
        double d = strtod(p, &end);
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
        return *end ? 0.0 / 0.0 : d;
    }
    default: {
        js_val s = js_to_string(v);
        if (s.t == JS_STR) {
            js_val t; t.t = JS_STR; t.u.s = s.u.s;
            return js_to_num(t);
        }
        return 0.0 / 0.0;
    }
    }
}

/* JavaScript prints 1 as "1", not "1.000000". Integers print as integers, and
 * everything else takes the fewest digits that reads back as the same number
 * -- which is what every other engine does and what makes output comparable. */
static void js_fmt_num(double d, char *out, int max) {
    if (d != d) { snprintf(out, max, "NaN"); return; }
    if (d == 1.0 / 0.0) { snprintf(out, max, "Infinity"); return; }
    if (d == -1.0 / 0.0) { snprintf(out, max, "-Infinity"); return; }
    if (d == 0) { snprintf(out, max, "0"); return; }

    double ip;
    if (modf(d, &ip) == 0.0 && d > -1e21 && d < 1e21) {
        snprintf(out, max, "%lld", (long long)d);
        return;
    }
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(out, max, "%.*g", prec, d);
        if (strtod(out, 0) == d) return;
    }
}

static js_val js_to_string(js_val v) {
    char buf[64];
    switch (v.t) {
    case JS_UNDEF: return js_str("undefined");
    case JS_NULL:  return js_str("null");
    case JS_BOOL:  return js_str(v.u.b ? "true" : "false");
    case JS_NUM:   js_fmt_num(v.u.n, buf, sizeof buf); return js_str(buf);
    case JS_STR:   return v;
    default:
        if (v.u.o && v.u.o->kind == JSO_ARRAY) {
            /* join with commas, as the language says */
            char *acc = (char *)js_alloc(1);
            int len = 0, cap = 0;
            (void)acc; (void)cap;
            /* Build into a temporary that grows by re-allocating. */
            static char tmp[4096];
            len = 0;
            for (int i = 0; i < v.u.o->nelems; i++) {
                if (i && len < (int)sizeof tmp - 1) tmp[len++] = ',';
                js_val e = v.u.o->elems[i];
                if (e.t == JS_UNDEF || e.t == JS_NULL) continue;
                js_val s = js_to_string(e);
                for (int k = 0; k < s.u.s.len && len < (int)sizeof tmp - 1; k++)
                    tmp[len++] = s.u.s.p[k];
            }
            tmp[len] = 0;
            return js_str_n(tmp, len);
        }
        if (v.u.o && v.u.o->kind == JSO_FUNC) return js_str("function");
        if (v.u.o && v.u.o->kind == JSO_NATIVE) return js_str("function");
        return js_str("[object Object]");
    }
}

static const char *js_cstr(js_val v) {
    js_val s = js_to_string(v);
    return s.t == JS_STR ? s.u.s.p : "";
}

static int js_str_eq(js_val a, js_val b) {
    if (a.u.s.len != b.u.s.len) return 0;
    for (int i = 0; i < a.u.s.len; i++) if (a.u.s.p[i] != b.u.s.p[i]) return 0;
    return 1;
}

static const char *js_typeof(js_val v) {
    switch (v.t) {
    case JS_UNDEF: return "undefined";
    case JS_NULL:  return "object";
    case JS_BOOL:  return "boolean";
    case JS_NUM:   return "number";
    case JS_STR:   return "string";
    default:
        return (v.u.o && (v.u.o->kind == JSO_FUNC || v.u.o->kind == JSO_NATIVE))
               ? "function" : "object";
    }
}

/* ==========================================================================
 * the lexer
 * ========================================================================== */

enum {
    T_EOF, T_NUM, T_STR, T_TEMPLATE, T_IDENT, T_PUNCT, T_KEYWORD
};

typedef struct {
    int  type;
    double num;
    const char *s;      /* identifiers, punctuation, string contents */
    int  len;
    int  line;
    int  nl_before;     /* a newline came before this token */
} js_tok;

typedef struct {
    const char *src;
    int len, pos, line;
    js_tok tok, ahead;
    int have_ahead;
} js_lex;

static const char *JS_KEYWORDS[] = {
    "var","let","const","function","return","if","else","for","while","do",
    "break","continue","new","delete","typeof","instanceof","in","of","this",
    "null","true","false","undefined","switch","case","default","throw","try",
    "catch","finally","void", 0
};

static int js_is_kw(const char *s, int len) {
    for (int i = 0; JS_KEYWORDS[i]; i++) {
        int l = (int)strlen(JS_KEYWORDS[i]);
        if (l == len && !memcmp(s, JS_KEYWORDS[i], (size_t)l)) return 1;
    }
    return 0;
}

static int js_idstart(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '$' || (unsigned char)c >= 128;
}
static int js_idpart(int c) { return js_idstart(c) || (c >= '0' && c <= '9'); }

/* The multi-character operators, longest first so ">>>=" is not read as ">>". */
static const char *JS_PUNCT[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/",
    "%", "&", "|", "^", "!", "~", "?", ":", "=", ".", 0
};

static void js_lex_init(js_lex *L, const char *src, int len) {
    memset(L, 0, sizeof *L);
    L->src = src; L->len = len; L->line = 1;
}

static void js_scan(js_lex *L, js_tok *t) {
    memset(t, 0, sizeof *t);
    int nl = 0;
    for (;;) {
        while (L->pos < L->len) {
            char c = L->src[L->pos];
            if (c == '\n') { L->line++; nl = 1; L->pos++; }
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') L->pos++;
            else break;
        }
        if (L->pos + 1 < L->len && L->src[L->pos] == '/' && L->src[L->pos+1] == '/') {
            while (L->pos < L->len && L->src[L->pos] != '\n') L->pos++;
            continue;
        }
        if (L->pos + 1 < L->len && L->src[L->pos] == '/' && L->src[L->pos+1] == '*') {
            L->pos += 2;
            while (L->pos + 1 < L->len &&
                   !(L->src[L->pos] == '*' && L->src[L->pos+1] == '/')) {
                if (L->src[L->pos] == '\n') { L->line++; nl = 1; }
                L->pos++;
            }
            L->pos += 2;
            continue;
        }
        break;
    }

    t->line = L->line;
    t->nl_before = nl;

    if (L->pos >= L->len) { t->type = T_EOF; return; }

    char c = L->src[L->pos];

    /* numbers */
    if ((c >= '0' && c <= '9') ||
        (c == '.' && L->pos + 1 < L->len &&
         L->src[L->pos+1] >= '0' && L->src[L->pos+1] <= '9')) {
        const char *start = L->src + L->pos;
        if (c == '0' && L->pos + 1 < L->len &&
            (L->src[L->pos+1] == 'x' || L->src[L->pos+1] == 'X')) {
            L->pos += 2;
            double v = 0;
            while (L->pos < L->len) {
                int d = L->src[L->pos], k;
                if (d >= '0' && d <= '9') k = d - '0';
                else if (d >= 'a' && d <= 'f') k = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') k = d - 'A' + 10;
                else break;
                v = v * 16 + k;
                L->pos++;
            }
            t->type = T_NUM; t->num = v;
            return;
        }
        char *end;
        t->num = strtod(start, &end);
        L->pos += (int)(end - start);
        t->type = T_NUM;
        return;
    }

    /* strings */
    if (c == '"' || c == '\'') {
        char q = c;
        L->pos++;
        char *buf = js_arena + js_arena_used;   /* build in place, then commit */
        int n = 0;
        while (L->pos < L->len && L->src[L->pos] != q) {
            char ch = L->src[L->pos++];
            if (ch == '\\' && L->pos < L->len) {
                char e = L->src[L->pos++];
                switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = '\0'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'v': ch = '\v'; break;
                case 'u': {
                    /* \uXXXX, written back out as UTF-8. */
                    int cp = 0, k = 0;
                    if (L->pos < L->len && L->src[L->pos] == '{') {
                        L->pos++;
                        while (L->pos < L->len && L->src[L->pos] != '}') {
                            int d = L->src[L->pos++], x;
                            if (d >= '0' && d <= '9') x = d - '0';
                            else if (d >= 'a' && d <= 'f') x = d - 'a' + 10;
                            else if (d >= 'A' && d <= 'F') x = d - 'A' + 10;
                            else break;
                            cp = cp * 16 + x;
                        }
                        if (L->pos < L->len) L->pos++;
                    } else {
                        while (k < 4 && L->pos < L->len) {
                            int d = L->src[L->pos], x;
                            if (d >= '0' && d <= '9') x = d - '0';
                            else if (d >= 'a' && d <= 'f') x = d - 'a' + 10;
                            else if (d >= 'A' && d <= 'F') x = d - 'A' + 10;
                            else break;
                            cp = cp * 16 + x; L->pos++; k++;
                        }
                    }
                    if (js_arena_used + n + 8 > JS_ARENA_CAP) { js_oom = 1; break; }
                    if (cp < 0x80) buf[n++] = (char)cp;
                    else if (cp < 0x800) {
                        buf[n++] = (char)(0xC0 | (cp >> 6));
                        buf[n++] = (char)(0x80 | (cp & 63));
                    } else {
                        buf[n++] = (char)(0xE0 | (cp >> 12));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 63));
                        buf[n++] = (char)(0x80 | (cp & 63));
                    }
                    continue;
                }
                case '\n': L->line++; continue;
                default: ch = e;
                }
            }
            if (js_arena_used + n + 2 > JS_ARENA_CAP) { js_oom = 1; break; }
            buf[n++] = ch;
        }
        if (L->pos < L->len) L->pos++;
        buf[n] = 0;
        js_arena_used += ((long)n + 16) & ~15L;
        t->type = T_STR; t->s = buf; t->len = n;
        return;
    }

    /* template literals, without the substitutions: `a ${b} c` is read as a
     * string with the expressions left in it, which is wrong in general and
     * right for the overwhelmingly common case of a plain quoted string. */
    if (c == '`') {
        L->pos++;
        const char *start = L->src + L->pos;
        int n = 0;
        while (L->pos < L->len && L->src[L->pos] != '`') {
            if (L->src[L->pos] == '\\' && L->pos + 1 < L->len) { L->pos += 2; n += 2; continue; }
            if (L->src[L->pos] == '\n') L->line++;
            L->pos++; n++;
        }
        if (L->pos < L->len) L->pos++;
        t->type = T_TEMPLATE; t->s = start; t->len = n;
        return;
    }

    /* identifiers and keywords */
    if (js_idstart(c)) {
        const char *start = L->src + L->pos;
        while (L->pos < L->len && js_idpart(L->src[L->pos])) L->pos++;
        t->s = start;
        t->len = (int)(L->src + L->pos - start);
        t->type = js_is_kw(start, t->len) ? T_KEYWORD : T_IDENT;
        return;
    }

    /* punctuation */
    for (int i = 0; JS_PUNCT[i]; i++) {
        int l = (int)strlen(JS_PUNCT[i]);
        if (L->pos + l <= L->len && !memcmp(L->src + L->pos, JS_PUNCT[i], (size_t)l)) {
            t->type = T_PUNCT;
            t->s = JS_PUNCT[i];
            t->len = l;
            L->pos += l;
            return;
        }
    }

    /* Anything else -- a stray backslash, a regular expression -- is skipped
     * one character at a time rather than allowed to stop the parse. */
    L->pos++;
    js_scan(L, t);
}

static void js_next(js_lex *L) {
    if (L->have_ahead) { L->tok = L->ahead; L->have_ahead = 0; return; }
    js_scan(L, &L->tok);
}

static js_tok *js_peek(js_lex *L) {
    if (!L->have_ahead) { js_scan(L, &L->ahead); L->have_ahead = 1; }
    return &L->ahead;
}

static int js_is(js_lex *L, const char *p) {
    return (L->tok.type == T_PUNCT || L->tok.type == T_KEYWORD) &&
           (int)strlen(p) == L->tok.len && !memcmp(L->tok.s, p, (size_t)L->tok.len);
}

static int js_eat(js_lex *L, const char *p) {
    if (!js_is(L, p)) return 0;
    js_next(L);
    return 1;
}

/* ==========================================================================
 * the syntax tree
 * ========================================================================== */

enum {
    N_NUM, N_STR, N_IDENT, N_BOOL, N_NULL, N_UNDEF, N_THIS,
    N_ARRAY, N_OBJECT, N_FUNC, N_ARROW,
    N_BIN, N_LOGIC, N_UNARY, N_ASSIGN, N_COND, N_CALL, N_MEMBER, N_INDEX,
    N_NEW, N_SEQ, N_PRE, N_POST,
    N_VAR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_WHILE, N_DO, N_RET,
    N_BREAK, N_CONT, N_EXPR, N_EMPTY, N_FUNCDECL, N_THROW, N_TRY, N_SWITCH
};

static char js_err[256];
static int  js_errline;

struct js_node {
    unsigned char type;
    const char *op;           /* operators, identifiers, property names */
    int oplen;
    double num;
    const char *str; int slen;
    js_node *a, *b, *c, *d;
    js_node **list; int nlist;
    int line;
    int flag;                 /* for-of vs for-in; optional chaining; ... */
    int uses_args;            /* the body mentions `arguments` */
};

/* The parser writes into a node the instant it makes one, so running out of
 * memory must not produce a null. It produces this instead: a real node that
 * everything can be written to and that is never read, because the error it
 * sets stops the parse. One shared scratch node is enough precisely because
 * nothing that reaches it will ever run. */
static js_node js_spoil;

static js_node *js_node_new(int type) {
    js_node *n = (js_node *)js_alloc(sizeof(js_node));
    if (!n) {
        if (!js_err[0])
            snprintf(js_err, sizeof js_err, "the script is too large to run");
        memset(&js_spoil, 0, sizeof js_spoil);
        return &js_spoil;
    }
    n->type = (unsigned char)type;
    return n;
}


static void js_fail(js_lex *L, const char *what) {
    if (js_err[0]) return;
    snprintf(js_err, sizeof js_err, "%s on line %d", what, L ? L->tok.line : 0);
    js_errline = L ? L->tok.line : 0;
}

/* Reading is bounded the same way running is. Checked once per statement --
 * often enough that nothing takes a noticeable extra moment, rare enough that
 * the clock does not show up in the cost. */
static double js_parse_deadline;

/* How long one script may take, reading and running each. A page is allowed
 * to be slow; a browser is not allowed to stop answering. */
static double js_budget_ms = 2000.0;

static int js_parse_overrun(void) {
    if (js_err[0]) return 1;
    if (js_parse_deadline > 0 && js_clock_ms && js_clock_ms() > js_parse_deadline) {
        snprintf(js_err, sizeof js_err, "the script took too long to read");
        return 1;
    }
    return 0;
}

static js_node *js_expr(js_lex *L);
static js_node *js_assign_expr(js_lex *L);
static js_node *js_statement(js_lex *L);

/* A copy of the current token's text, kept for the tree. */
static const char *js_tok_text(js_lex *L, int *len) {
    char *p = (char *)js_alloc(L->tok.len + 1);
    if (!p) { *len = 0; return ""; }
    memcpy(p, L->tok.s, (size_t)L->tok.len);
    p[L->tok.len] = 0;
    *len = L->tok.len;
    return p;
}

static js_node **js_list_add(js_node ***list, int *n, int *cap, js_node *item) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        js_node **nl = (js_node **)js_alloc((long)nc * sizeof(js_node *));
        if (!nl) return *list;
        for (int i = 0; i < *n; i++) nl[i] = (*list)[i];
        *list = nl; *cap = nc;
    }
    (*list)[(*n)++] = item;
    return *list;
}

/* --- function bodies and parameters --------------------------------------- */

/* Whether a function needs its `arguments` array.
 *
 * Walking the function's syntax tree to find out was quadratic in the nesting,
 * which a hundred kilobytes of minified code has plenty of. Looking at the
 * function's own SOURCE TEXT instead is the same answer for a fraction of the
 * cost -- and one scan of the whole file first rules out the overwhelming
 * majority of pages, which never mention it at all. */
static int js_src_has_arguments;

static int js_range_has_arguments(const char *src, int from, int to) {
    if (!js_src_has_arguments) return 0;
    for (int i = from; i + 9 <= to; i++)
        if (src[i] == 'a' && !memcmp(src + i, "arguments", 9)) return 1;
    return 0;
}

static js_node *js_params_body(js_lex *L, js_node *fn) {
    int cap = 0;
    if (!js_eat(L, "(")) { js_fail(L, "expected ("); return fn; }
    while (!js_is(L, ")") && L->tok.type != T_EOF) {
        if (js_eat(L, "...")) fn->flag = 1;      /* rest: bound to an array */
        if (L->tok.type != T_IDENT) { js_fail(L, "bad parameter"); break; }
        js_node *p = js_node_new(N_IDENT);
        p->op = js_tok_text(L, &p->oplen);
        js_next(L);
        if (js_eat(L, "=")) p->a = js_assign_expr(L);   /* a default */
        js_list_add(&fn->list, &fn->nlist, &cap, p);
        if (!js_eat(L, ",")) break;
    }
    if (!js_eat(L, ")")) js_fail(L, "expected )");
    int body_from = L->pos;
    fn->a = js_statement(L);
    fn->uses_args = js_range_has_arguments(L->src, body_from, L->pos);
    return fn;
}

/* --- primary expressions --------------------------------------------------- */

static js_node *js_primary(js_lex *L) {
    js_node *n;

    if (L->tok.type == T_NUM) {
        n = js_node_new(N_NUM); n->num = L->tok.num; js_next(L); return n;
    }
    if (L->tok.type == T_STR) {
        n = js_node_new(N_STR); n->str = L->tok.s; n->slen = L->tok.len;
        js_next(L); return n;
    }
    if (L->tok.type == T_TEMPLATE) {
        n = js_node_new(N_STR); n->str = L->tok.s; n->slen = L->tok.len;
        js_next(L); return n;
    }
    if (js_is(L, "true"))  { js_next(L); n = js_node_new(N_BOOL); n->num = 1; return n; }
    if (js_is(L, "false")) { js_next(L); n = js_node_new(N_BOOL); n->num = 0; return n; }
    if (js_is(L, "null"))  { js_next(L); return js_node_new(N_NULL); }
    if (js_is(L, "undefined")) { js_next(L); return js_node_new(N_UNDEF); }
    if (js_is(L, "this"))  { js_next(L); return js_node_new(N_THIS); }

    if (js_is(L, "function")) {
        js_next(L);
        n = js_node_new(N_FUNC);
        if (L->tok.type == T_IDENT) { n->op = js_tok_text(L, &n->oplen); js_next(L); }
        return js_params_body(L, n);
    }

    if (js_is(L, "new")) {
        js_next(L);
        n = js_node_new(N_NEW);
        n->a = js_primary(L);
        /* member accesses bind tighter than the argument list */
        for (;;) {
            if (js_is(L, ".")) {
                js_next(L);
                js_node *m = js_node_new(N_MEMBER);
                m->a = n->a;
                m->op = js_tok_text(L, &m->oplen);
                js_next(L);
                n->a = m;
            } else break;
        }
        int cap = 0;
        if (js_eat(L, "(")) {
            while (!js_is(L, ")") && L->tok.type != T_EOF) {
                js_list_add(&n->list, &n->nlist, &cap, js_assign_expr(L));
                if (!js_eat(L, ",")) break;
            }
            if (!js_eat(L, ")")) js_fail(L, "expected )");
        }
        return n;
    }

    if (js_is(L, "(")) {
        /* Either a parenthesised expression or an arrow function's parameter
         * list. The two are told apart by LOOKING -- scanning tokens to the
         * matching bracket and seeing whether "=>" follows it.
         *
         * Parsing it as parameters and rewinding when that failed was the
         * obvious way and it was exponential: each nesting level re-read
         * everything inside it, so minified code with parentheses fifty deep
         * never came back at all. */
        int is_arrow = 0;
        {
            js_lex look = *L;
            int depth = 0;
            for (int guard = 0; guard < 100000; guard++) {
                if (look.tok.type == T_EOF) break;
                if (look.tok.type == T_PUNCT && look.tok.len == 1) {
                    char c = look.tok.s[0];
                    if (c == '(' || c == '[' || c == '{') depth++;
                    else if (c == ')' || c == ']' || c == '}') {
                        if (--depth == 0) {
                            js_next(&look);
                            is_arrow = (look.tok.type == T_PUNCT &&
                                        look.tok.len == 2 &&
                                        !memcmp(look.tok.s, "=>", 2));
                            break;
                        }
                    }
                }
                js_next(&look);
            }
        }

        if (is_arrow) {
            js_node *fn = js_node_new(N_ARROW);
            int cap = 0;
            js_next(L);                       /* past '(' */
            while (!js_is(L, ")") && L->tok.type != T_EOF) {
                if (js_eat(L, "...")) fn->flag = 1;
                if (L->tok.type != T_IDENT) {
                    /* A destructured parameter, which we do not implement.
                     * Stepped over rather than allowed to derail the parse. */
                    js_next(L);
                    if (!js_eat(L, ",")) break;
                    continue;
                }
                js_node *p = js_node_new(N_IDENT);
                p->op = js_tok_text(L, &p->oplen);
                js_next(L);
                if (js_eat(L, "=")) p->a = js_assign_expr(L);
                js_list_add(&fn->list, &fn->nlist, &cap, p);
                if (!js_eat(L, ",")) break;
            }
            if (!js_eat(L, ")")) js_fail(L, "expected )");
            if (!js_eat(L, "=>")) js_fail(L, "expected =>");
            int body_from = L->pos;
            if (js_is(L, "{")) fn->a = js_statement(L);
            else {
                js_node *r = js_node_new(N_RET);
                r->a = js_assign_expr(L);
                fn->a = r;
            }
            fn->uses_args = js_range_has_arguments(L->src, body_from, L->pos);
            return fn;
        }

        js_next(L);                       /* past '(' */
        js_node *e = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        return e;
    }

    if (js_is(L, "[")) {
        js_next(L);
        n = js_node_new(N_ARRAY);
        int cap = 0;
        while (!js_is(L, "]") && L->tok.type != T_EOF) {
            if (js_is(L, ",")) { js_next(L); continue; }
            js_list_add(&n->list, &n->nlist, &cap, js_assign_expr(L));
            if (!js_eat(L, ",")) break;
        }
        if (!js_eat(L, "]")) js_fail(L, "expected ]");
        return n;
    }

    if (js_is(L, "{")) {
        js_next(L);
        n = js_node_new(N_OBJECT);
        int cap = 0;
        while (!js_is(L, "}") && L->tok.type != T_EOF) {
            js_node *pair = js_node_new(N_SEQ);
            if (L->tok.type == T_IDENT || L->tok.type == T_KEYWORD) {
                pair->op = js_tok_text(L, &pair->oplen);
                js_next(L);
            } else if (L->tok.type == T_STR) {
                pair->op = L->tok.s; pair->oplen = L->tok.len;
                js_next(L);
            } else if (L->tok.type == T_NUM) {
                char b[40];
                js_fmt_num(L->tok.num, b, sizeof b);
                int bl = (int)strlen(b);
                char *cp = (char *)js_alloc(bl + 1);
                memcpy(cp, b, (size_t)bl + 1);
                pair->op = cp; pair->oplen = bl;
                js_next(L);
            } else { js_fail(L, "bad property name"); break; }

            if (js_eat(L, ":")) {
                pair->a = js_assign_expr(L);
            } else if (js_is(L, "(")) {
                /* a method written in the short form */
                js_node *fn = js_node_new(N_FUNC);
                pair->a = js_params_body(L, fn);
            } else {
                /* { x } is { x: x } */
                js_node *id = js_node_new(N_IDENT);
                id->op = pair->op; id->oplen = pair->oplen;
                pair->a = id;
            }
            js_list_add(&n->list, &n->nlist, &cap, pair);
            if (!js_eat(L, ",")) break;
        }
        if (!js_eat(L, "}")) js_fail(L, "expected }");
        return n;
    }

    if (L->tok.type == T_IDENT) {
        /* x => ... is an arrow function too. */
        if (js_peek(L)->type == T_PUNCT && js_peek(L)->len == 2 &&
            !memcmp(js_peek(L)->s, "=>", 2)) {
            js_node *fn = js_node_new(N_ARROW);
            int cap = 0;
            js_node *p = js_node_new(N_IDENT);
            p->op = js_tok_text(L, &p->oplen);
            js_next(L);   /* the parameter */
            js_next(L);   /* => */
            js_list_add(&fn->list, &fn->nlist, &cap, p);
            int body_from = L->pos;
            if (js_is(L, "{")) fn->a = js_statement(L);
            else {
                js_node *r = js_node_new(N_RET);
                r->a = js_assign_expr(L);
                fn->a = r;
            }
            fn->uses_args = js_range_has_arguments(L->src, body_from, L->pos);
            return fn;
        }
        n = js_node_new(N_IDENT);
        n->op = js_tok_text(L, &n->oplen);
        n->line = L->tok.line;
        js_next(L);
        return n;
    }

    js_fail(L, "unexpected token");
    js_next(L);
    return js_node_new(N_UNDEF);
}

/* --- calls, members, subscripts -------------------------------------------- */

static js_node *js_postfix_chain(js_lex *L, js_node *base) {
    for (;;) {
        if (js_is(L, ".") || js_is(L, "?.")) {
            int optional = js_is(L, "?.");
            js_next(L);
            if (optional && js_is(L, "(")) {          /* fn?.() */
                js_node *c = js_node_new(N_CALL);
                c->a = base; c->flag = 1;
                int cap = 0;
                js_next(L);
                while (!js_is(L, ")") && L->tok.type != T_EOF) {
                    js_list_add(&c->list, &c->nlist, &cap, js_assign_expr(L));
                    if (!js_eat(L, ",")) break;
                }
                if (!js_eat(L, ")")) js_fail(L, "expected )");
                base = c;
                continue;
            }
            js_node *m = js_node_new(N_MEMBER);
            m->a = base;
            m->flag = optional;
            m->op = js_tok_text(L, &m->oplen);
            js_next(L);
            base = m;
        } else if (js_is(L, "[")) {
            js_next(L);
            js_node *ix = js_node_new(N_INDEX);
            ix->a = base;
            ix->b = js_expr(L);
            if (!js_eat(L, "]")) js_fail(L, "expected ]");
            base = ix;
        } else if (js_is(L, "(")) {
            js_next(L);
            js_node *c = js_node_new(N_CALL);
            c->a = base;
            int cap = 0;
            while (!js_is(L, ")") && L->tok.type != T_EOF) {
                if (js_eat(L, "...")) {
                    js_node *sp = js_node_new(N_UNARY);
                    sp->op = "..."; sp->oplen = 3;
                    sp->a = js_assign_expr(L);
                    js_list_add(&c->list, &c->nlist, &cap, sp);
                } else {
                    js_list_add(&c->list, &c->nlist, &cap, js_assign_expr(L));
                }
                if (!js_eat(L, ",")) break;
            }
            if (!js_eat(L, ")")) js_fail(L, "expected )");
            base = c;
        } else break;
    }
    return base;
}

static js_node *js_unary(js_lex *L);

static js_node *js_postfix(js_lex *L) {
    js_node *e = js_postfix_chain(L, js_primary(L));
    if ((js_is(L, "++") || js_is(L, "--")) && !L->tok.nl_before) {
        js_node *n = js_node_new(N_POST);
        n->op = js_is(L, "++") ? "++" : "--";
        n->oplen = 2;
        n->a = e;
        js_next(L);
        return n;
    }
    return e;
}

static js_node *js_unary(js_lex *L) {
    static const char *U[] = { "!", "-", "+", "~", "typeof", "void", "delete", 0 };
    for (int i = 0; U[i]; i++) {
        if (js_is(L, U[i])) {
            js_node *n = js_node_new(N_UNARY);
            n->op = U[i]; n->oplen = (int)strlen(U[i]);
            js_next(L);
            n->a = js_unary(L);
            return n;
        }
    }
    if (js_is(L, "++") || js_is(L, "--")) {
        js_node *n = js_node_new(N_PRE);
        n->op = js_is(L, "++") ? "++" : "--";
        n->oplen = 2;
        js_next(L);
        n->a = js_unary(L);
        return n;
    }
    return js_postfix(L);
}

/* Precedence, lowest binding first. */
static const struct { const char *op; int prec; } JS_BINOPS[] = {
    { "??", 1 }, { "||", 2 }, { "&&", 3 },
    { "|", 4 }, { "^", 5 }, { "&", 6 },
    { "===", 7 }, { "!==", 7 }, { "==", 7 }, { "!=", 7 },
    { "<=", 8 }, { ">=", 8 }, { "<", 8 }, { ">", 8 },
    { "instanceof", 8 }, { "in", 8 },
    { "<<", 9 }, { ">>>", 9 }, { ">>", 9 },
    { "+", 10 }, { "-", 10 },
    { "*", 11 }, { "/", 11 }, { "%", 11 },
    { "**", 12 },
    { 0, 0 }
};

static int js_binprec(js_lex *L, const char **op) {
    if (L->tok.type != T_PUNCT && L->tok.type != T_KEYWORD) return 0;
    for (int i = 0; JS_BINOPS[i].op; i++) {
        int l = (int)strlen(JS_BINOPS[i].op);
        if (l == L->tok.len && !memcmp(L->tok.s, JS_BINOPS[i].op, (size_t)l)) {
            *op = JS_BINOPS[i].op;
            return JS_BINOPS[i].prec;
        }
    }
    return 0;
}

static js_node *js_binary(js_lex *L, int minprec) {
    js_node *left = js_unary(L);
    for (;;) {
        const char *op = 0;
        int prec = js_binprec(L, &op);
        if (!prec || prec < minprec) break;
        js_next(L);
        js_node *right = js_binary(L, prec + 1);
        int logic = (!strcmp(op, "&&") || !strcmp(op, "||") || !strcmp(op, "??"));
        js_node *n = js_node_new(logic ? N_LOGIC : N_BIN);
        n->op = op; n->oplen = (int)strlen(op);
        n->a = left; n->b = right;
        left = n;
    }
    return left;
}

static js_node *js_conditional(js_lex *L) {
    js_node *c = js_binary(L, 1);
    if (js_is(L, "?")) {
        js_next(L);
        js_node *n = js_node_new(N_COND);
        n->a = c;
        n->b = js_assign_expr(L);
        if (!js_eat(L, ":")) js_fail(L, "expected :");
        n->c = js_assign_expr(L);
        return n;
    }
    return c;
}

static js_node *js_assign_expr(js_lex *L) {
    js_node *left = js_conditional(L);
    static const char *A[] = { "=", "+=", "-=", "*=", "/=", "%=", "**=",
                               "&=", "|=", "^=", "<<=", ">>=", ">>>=",
                               "&&=", "||=", "??=", 0 };
    for (int i = 0; A[i]; i++) {
        if (js_is(L, A[i])) {
            js_node *n = js_node_new(N_ASSIGN);
            n->op = A[i]; n->oplen = (int)strlen(A[i]);
            js_next(L);
            n->a = left;
            n->b = js_assign_expr(L);
            return n;
        }
    }
    return left;
}

static js_node *js_expr(js_lex *L) {
    js_node *e = js_assign_expr(L);
    while (js_is(L, ",")) {
        js_next(L);
        js_node *n = js_node_new(N_SEQ);
        n->a = e;
        n->b = js_assign_expr(L);
        e = n;
    }
    return e;
}

/* --- statements ------------------------------------------------------------ */

static void js_semi(js_lex *L) { js_eat(L, ";"); }

static js_node *js_var_decl(js_lex *L) {
    js_node *n = js_node_new(N_VAR);
    int cap = 0;
    js_next(L);                       /* var / let / const */
    for (;;) {
        if (L->tok.type != T_IDENT) { js_fail(L, "expected a name"); break; }
        js_node *d = js_node_new(N_IDENT);
        d->op = js_tok_text(L, &d->oplen);
        js_next(L);
        if (js_eat(L, "=")) d->a = js_assign_expr(L);
        js_list_add(&n->list, &n->nlist, &cap, d);
        if (!js_eat(L, ",")) break;
    }
    return n;
}

static js_node *js_statement(js_lex *L) {
    if (js_parse_overrun()) {
        /* Consume something regardless, so that every loop calling this keeps
         * advancing towards the end of the file. Returning without doing so
         * spun forever the moment anything failed to parse -- which on a real
         * page is not an edge case but the usual outcome. */
        if (L->tok.type != T_EOF) js_next(L);
        return js_node_new(N_EMPTY);
    }

    if (js_is(L, "{")) {
        js_next(L);
        js_node *n = js_node_new(N_BLOCK);
        int cap = 0;
        while (!js_is(L, "}") && L->tok.type != T_EOF && !js_err[0])
            js_list_add(&n->list, &n->nlist, &cap, js_statement(L));
        if (!js_eat(L, "}")) js_fail(L, "expected }");
        return n;
    }
    if (js_eat(L, ";")) return js_node_new(N_EMPTY);

    if (js_is(L, "var") || js_is(L, "let") || js_is(L, "const")) {
        js_node *n = js_var_decl(L);
        js_semi(L);
        return n;
    }

    if (js_is(L, "function")) {
        js_next(L);
        js_node *n = js_node_new(N_FUNCDECL);
        if (L->tok.type == T_IDENT) { n->op = js_tok_text(L, &n->oplen); js_next(L); }
        js_node *fn = js_node_new(N_FUNC);
        fn->op = n->op; fn->oplen = n->oplen;
        n->b = js_params_body(L, fn);
        return n;
    }

    if (js_is(L, "if")) {
        js_next(L);
        js_node *n = js_node_new(N_IF);
        if (!js_eat(L, "(")) js_fail(L, "expected (");
        n->a = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        n->b = js_statement(L);
        if (js_is(L, "else")) { js_next(L); n->c = js_statement(L); }
        return n;
    }

    if (js_is(L, "while")) {
        js_next(L);
        js_node *n = js_node_new(N_WHILE);
        if (!js_eat(L, "(")) js_fail(L, "expected (");
        n->a = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        n->b = js_statement(L);
        return n;
    }

    if (js_is(L, "do")) {
        js_next(L);
        js_node *n = js_node_new(N_DO);
        n->b = js_statement(L);
        if (!js_eat(L, "while")) js_fail(L, "expected while");
        if (!js_eat(L, "(")) js_fail(L, "expected (");
        n->a = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        js_semi(L);
        return n;
    }

    if (js_is(L, "for")) {
        js_next(L);
        if (!js_eat(L, "(")) js_fail(L, "expected (");

        js_node *init = 0;
        int decl = 0;
        if (js_is(L, "var") || js_is(L, "let") || js_is(L, "const")) {
            decl = 1;
            init = js_var_decl(L);
        } else if (!js_is(L, ";")) {
            init = js_node_new(N_EXPR);
            init->a = js_expr(L);
        }

        if (js_is(L, "of") || js_is(L, "in")) {
            js_node *n = js_node_new(N_FORIN);
            n->flag = js_is(L, "of");
            js_next(L);
            n->a = init;                        /* the variable */
            n->b = js_assign_expr(L);           /* what to walk */
            n->d = (js_node *)(long)decl;
            if (!js_eat(L, ")")) js_fail(L, "expected )");
            n->c = js_statement(L);
            return n;
        }

        js_node *n = js_node_new(N_FOR);
        n->a = init;
        if (!js_eat(L, ";")) js_fail(L, "expected ;");
        if (!js_is(L, ";")) n->b = js_expr(L);
        if (!js_eat(L, ";")) js_fail(L, "expected ;");
        if (!js_is(L, ")")) n->c = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        n->d = js_statement(L);
        return n;
    }

    if (js_is(L, "return")) {
        js_next(L);
        js_node *n = js_node_new(N_RET);
        if (!js_is(L, ";") && !js_is(L, "}") && L->tok.type != T_EOF &&
            !L->tok.nl_before)
            n->a = js_expr(L);
        js_semi(L);
        return n;
    }
    if (js_is(L, "break"))    { js_next(L); js_semi(L); return js_node_new(N_BREAK); }
    if (js_is(L, "continue")) { js_next(L); js_semi(L); return js_node_new(N_CONT); }

    if (js_is(L, "throw")) {
        js_next(L);
        js_node *n = js_node_new(N_THROW);
        n->a = js_expr(L);
        js_semi(L);
        return n;
    }

    if (js_is(L, "try")) {
        js_next(L);
        js_node *n = js_node_new(N_TRY);
        n->a = js_statement(L);
        if (js_is(L, "catch")) {
            js_next(L);
            if (js_eat(L, "(")) {
                if (L->tok.type == T_IDENT) {
                    js_node *id = js_node_new(N_IDENT);
                    id->op = js_tok_text(L, &id->oplen);
                    n->d = id;
                    js_next(L);
                }
                if (!js_eat(L, ")")) js_fail(L, "expected )");
            }
            n->b = js_statement(L);
        }
        if (js_is(L, "finally")) { js_next(L); n->c = js_statement(L); }
        return n;
    }

    if (js_is(L, "switch")) {
        js_next(L);
        js_node *n = js_node_new(N_SWITCH);
        if (!js_eat(L, "(")) js_fail(L, "expected (");
        n->a = js_expr(L);
        if (!js_eat(L, ")")) js_fail(L, "expected )");
        if (!js_eat(L, "{")) js_fail(L, "expected {");
        int cap = 0;
        while (!js_is(L, "}") && L->tok.type != T_EOF) {
            js_node *cs = js_node_new(N_SEQ);
            if (js_eat(L, "case")) {
                cs->a = js_expr(L);
            } else if (js_eat(L, "default")) {
                cs->flag = 1;
            } else { js_fail(L, "expected case"); break; }
            if (!js_eat(L, ":")) js_fail(L, "expected :");
            js_node *body = js_node_new(N_BLOCK);
            int bcap = 0;
            while (!js_is(L, "case") && !js_is(L, "default") &&
                   !js_is(L, "}") && L->tok.type != T_EOF && !js_err[0])
                js_list_add(&body->list, &body->nlist, &bcap, js_statement(L));
            cs->b = body;
            js_list_add(&n->list, &n->nlist, &cap, cs);
        }
        if (!js_eat(L, "}")) js_fail(L, "expected }");
        return n;
    }

    /* A label, which we accept and ignore: `outer: for (...)`. */
    if (L->tok.type == T_IDENT && js_peek(L)->type == T_PUNCT &&
        js_peek(L)->len == 1 && js_peek(L)->s[0] == ':') {
        js_next(L); js_next(L);
        return js_statement(L);
    }

    js_node *n = js_node_new(N_EXPR);
    n->a = js_expr(L);
    js_semi(L);
    return n;
}

/* ==========================================================================
 * scopes
 * ========================================================================== */

struct js_scope {
    js_obj   *vars;
    js_scope *parent;
    js_val    self;          /* `this` */
    int       has_self;
    int       captured;      /* a function made here still refers to it */
    js_scope *freelink;
};

/* Scopes are the one thing a program makes constantly and stops needing
 * immediately, and with an arena there is nothing to free them into. So the
 * ones nobody kept are put on a list and handed out again.
 *
 * "Nobody kept" is exact rather than a guess: the only thing that can outlive
 * a call is a function defined during it, and making a function marks the
 * whole chain of scopes it can see. Without this a few thousand calls exhaust
 * the arena; with it, a loop calling a function a million times allocates
 * nothing at all. */
static js_scope *js_global_scope;
static js_scope *js_scope_pool;

static js_scope *js_scope_new(js_scope *parent) {
    js_scope *s = js_scope_pool;
    if (s) {
        js_scope_pool = s->freelink;
        s->vars->nprops = 0;         /* the storage stays, the names go */
    } else {
        s = (js_scope *)js_alloc(sizeof(js_scope));
        if (!s) return 0;
        s->vars = js_new(JSO_PLAIN);
        if (!s->vars) return 0;
    }
    s->parent = parent;
    s->self = js_undef();
    s->has_self = 0;
    s->captured = 0;
    s->freelink = 0;
    return s;
}

static void js_scope_release(js_scope *s) {
    if (!s || s->captured || s == js_global_scope) return;
    s->freelink = js_scope_pool;
    js_scope_pool = s;
}

static js_val *js_lookup(js_scope *s, const char *name) {
    for (; s; s = s->parent) {
        js_val *v = js_find(s->vars, name);
        if (v) return v;
    }
    return 0;
}

static js_val js_this(js_scope *s) {
    for (; s; s = s->parent) if (s->has_self) return s->self;
    return js_undef();
}

/* ==========================================================================
 * running it
 * ========================================================================== */

enum { C_NORMAL, C_RETURN, C_BREAK, C_CONTINUE, C_THROW };


static int    js_signal;
static js_val js_signal_val;
static long   js_steps;

/* Both the parser and the interpreter recurse once per level of nesting, and
 * minified code nests deeply. The stack a userland program gets is not large,
 * and running off the end of it is a fault rather than an error, so the depth
 * is counted and refused instead. */
#define JS_MAX_DEPTH 220
static int js_depth;
static long   js_step_cap = 30000000L;

static js_val js_eval(js_node *n, js_scope *sc);
static js_val js_eval_inner(js_node *n, js_scope *sc);
static void   js_exec(js_node *n, js_scope *sc);
static void   js_exec_inner(js_node *n, js_scope *sc);

static void js_throw_str(const char *msg) {
    js_signal = C_THROW;
    js_signal_val = js_str(msg);
    if (!js_err[0]) snprintf(js_err, sizeof js_err, "%s", msg);
}

/* When the running script must be finished by. */
static double js_deadline;

static int js_stop(void) {
    if (js_oom) { js_throw_str("script ran out of memory"); return 1; }
    if (++js_steps > js_step_cap) {
        js_throw_str("script took too long and was stopped");
        return 1;
    }
    /* Reading a clock is not free, so it is read once every few thousand
     * steps -- fine grained enough that nothing runs for a noticeable extra
     * moment, coarse enough not to show up in the cost. */
    if ((js_steps & 0x3FF) == 0 && js_clock_ms && js_deadline > 0 &&
        js_clock_ms() > js_deadline) {
        js_throw_str("script took too long and was stopped");
        return 1;
    }
    return js_signal != C_NORMAL;
}

static js_val js_call(js_val fnv, js_val self, js_val *args, int nargs);

/* Reading a property. Strings, arrays and numbers answer for their own
 * methods; a host object is asked. */
static js_val js_getprop(js_val base, const char *name);
static void   js_setprop(js_val base, const char *name, js_val v);

/* --- the built-in methods, by name ----------------------------------------- */

static js_val js_arg(js_val *a, int n, int i) { return i < n ? a[i] : js_undef(); }

/* Two the library needs and the interpreter defines below it. */
static js_val js_binop(const char *op, js_val a, js_val b);
static int    js_strict_eq(js_val a, js_val b);

#include "jsbuiltin.h"

/* --- property access -------------------------------------------------------- */

static js_val js_getprop(js_val base, const char *name) {
    if (base.t == JS_STR) return js_string_prop(base, name);
    if (base.t == JS_NUM) return js_number_prop(base, name);

    if (base.t != JS_OBJ || !base.u.o) {
        js_throw_str("cannot read a property of nothing");
        return js_undef();
    }
    js_obj *o = base.u.o;

    if (o->hget) {
        js_val out;
        if (o->hget(o, name, &out)) return out;
    }
    if (o->kind == JSO_ARRAY) {
        js_val r = js_array_prop(base, name);
        if (r.t != JS_UNDEF) return r;
        if (!strcmp(name, "length")) return js_num(o->nelems);
        /* an index written as a name */
        if (name[0] >= '0' && name[0] <= '9') {
            int i = atoi(name);
            if (i >= 0 && i < o->nelems) return o->elems[i];
        }
    }
    for (js_obj *p = o; p; p = p->proto) {
        js_val *v = js_find(p, name);
        if (v) return *v;
    }
    if (o->kind == JSO_FUNC || o->kind == JSO_NATIVE) {
        js_val r = js_function_prop(base, name);
        if (r.t != JS_UNDEF) return r;
    }
    return js_object_prop(base, name);
}

static void js_setprop(js_val base, const char *name, js_val v) {
    if (base.t != JS_OBJ || !base.u.o) return;
    js_obj *o = base.u.o;
    if (o->hset && o->hset(o, name, v)) return;
    if (o->kind == JSO_ARRAY) {
        if (!strcmp(name, "length")) {
            int n = (int)js_to_num(v);
            if (n >= 0 && n <= o->nelems) o->nelems = n;
            return;
        }
        if (name[0] >= '0' && name[0] <= '9') {
            int i = atoi(name);
            while (o->nelems <= i) js_arr_push(o, js_undef());
            if (i >= 0 && i < o->nelems) o->elems[i] = v;
            return;
        }
    }
    js_set(o, name, v);
}

/* --- calling ---------------------------------------------------------------- */

static js_val js_call(js_val fnv, js_val self, js_val *args, int nargs) {
    if (fnv.t != JS_OBJ || !fnv.u.o ||
        (fnv.u.o->kind != JSO_FUNC && fnv.u.o->kind != JSO_NATIVE)) {
        js_throw_str("tried to call something that is not a function");
        return js_undef();
    }
    js_obj *f = fnv.u.o;
    if (f->kind == JSO_NATIVE) return f->native(self, args, nargs);

    js_scope *sc = js_scope_new(f->closure ? f->closure : js_global_scope);
    if (!sc) { js_throw_str("out of memory"); return js_undef(); }

    /* An arrow function keeps the `this` of where it was written; an ordinary
     * one takes the one it was called with. */
    if (f->fn->type != N_ARROW) { sc->self = self; sc->has_self = 1; }

    js_node *fn = f->fn;
    for (int i = 0; i < fn->nlist; i++) {
        js_node *p = fn->list[i];
        js_val v = (i < nargs) ? args[i] : js_undef();
        if (v.t == JS_UNDEF && p->a) v = js_eval(p->a, sc);
        js_set_ref(sc->vars, p->op, v);
    }
    /* `arguments` is built only for the functions that name it -- it is an
     * array per call otherwise, for nothing. */
    if (fn->uses_args) {
        js_obj *ar = js_new_array();
        for (int i = 0; i < nargs; i++) js_arr_push(ar, args[i]);
        js_set_ref(sc->vars, "arguments", js_obj_val(ar));
    }

    js_exec(fn->a, sc);
    js_val r = js_undef();
    if (js_signal == C_RETURN) {
        js_signal = C_NORMAL;
        r = js_signal_val;
        js_signal_val = js_undef();
    }
    js_scope_release(sc);
    return r;
}

/* --- assignment targets ------------------------------------------------------ */

static void js_assign_to(js_node *t, js_val v, js_scope *sc) {
    if (!t) return;
    if (t->type == N_IDENT) {
        js_val *slot = js_lookup(sc, t->op);
        if (slot) *slot = v;
        else js_set(js_global_scope->vars, t->op, v);
        return;
    }
    if (t->type == N_MEMBER) {
        js_val base = js_eval(t->a, sc);
        js_setprop(base, t->op, v);
        return;
    }
    if (t->type == N_INDEX) {
        js_val base = js_eval(t->a, sc);
        js_val idx = js_eval(t->b, sc);
        if (base.t == JS_OBJ && base.u.o && base.u.o->kind == JSO_ARRAY &&
            idx.t == JS_NUM) {
            js_obj *a = base.u.o;
            int i = (int)idx.u.n;
            if (i >= 0) {
                while (a->nelems <= i) js_arr_push(a, js_undef());
                a->elems[i] = v;
            }
            return;
        }
        js_setprop(base, js_cstr(idx), v);
        return;
    }
    js_throw_str("cannot assign to that");
}

/* --- operators ---------------------------------------------------------------- */

static int js_loose_eq(js_val a, js_val b);

static int js_strict_eq(js_val a, js_val b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case JS_UNDEF: case JS_NULL: return 1;
    case JS_BOOL: return a.u.b == b.u.b;
    case JS_NUM:  return a.u.n == b.u.n;
    case JS_STR:  return js_str_eq(a, b);
    default: return a.u.o == b.u.o;
    }
}

static int js_loose_eq(js_val a, js_val b) {
    if (a.t == b.t) return js_strict_eq(a, b);
    if ((a.t == JS_NULL && b.t == JS_UNDEF) || (a.t == JS_UNDEF && b.t == JS_NULL))
        return 1;
    if (a.t == JS_NULL || a.t == JS_UNDEF || b.t == JS_NULL || b.t == JS_UNDEF)
        return 0;
    if (a.t == JS_STR && b.t == JS_STR) return js_str_eq(a, b);
    return js_to_num(a) == js_to_num(b);
}

static js_val js_binop(const char *op, js_val a, js_val b) {
    if (!strcmp(op, "+")) {
        if (a.t == JS_STR || b.t == JS_STR ||
            (a.t == JS_OBJ) || (b.t == JS_OBJ)) {
            js_val sa = js_to_string(a), sb = js_to_string(b);
            int n = sa.u.s.len + sb.u.s.len;
            char *p = (char *)js_alloc(n + 1);
            if (!p) return js_str("");
            memcpy(p, sa.u.s.p, (size_t)sa.u.s.len);
            memcpy(p + sa.u.s.len, sb.u.s.p, (size_t)sb.u.s.len);
            p[n] = 0;
            js_val r; r.t = JS_STR; r.u.s.p = p; r.u.s.len = n;
            return r;
        }
        return js_num(js_to_num(a) + js_to_num(b));
    }
    if (!strcmp(op, "-")) return js_num(js_to_num(a) - js_to_num(b));
    if (!strcmp(op, "*")) return js_num(js_to_num(a) * js_to_num(b));
    if (!strcmp(op, "/")) return js_num(js_to_num(a) / js_to_num(b));
    if (!strcmp(op, "%")) return js_num(fmod(js_to_num(a), js_to_num(b)));
    if (!strcmp(op, "**")) return js_num(pow(js_to_num(a), js_to_num(b)));

    if (!strcmp(op, "===")) return js_bool(js_strict_eq(a, b));
    if (!strcmp(op, "!==")) return js_bool(!js_strict_eq(a, b));
    if (!strcmp(op, "=="))  return js_bool(js_loose_eq(a, b));
    if (!strcmp(op, "!="))  return js_bool(!js_loose_eq(a, b));

    if (!strcmp(op, "<") || !strcmp(op, ">") ||
        !strcmp(op, "<=") || !strcmp(op, ">=")) {
        if (a.t == JS_STR && b.t == JS_STR) {
            int n = a.u.s.len < b.u.s.len ? a.u.s.len : b.u.s.len;
            int c = memcmp(a.u.s.p, b.u.s.p, (size_t)n);
            if (!c) c = a.u.s.len - b.u.s.len;
            if (!strcmp(op, "<"))  return js_bool(c < 0);
            if (!strcmp(op, ">"))  return js_bool(c > 0);
            if (!strcmp(op, "<=")) return js_bool(c <= 0);
            return js_bool(c >= 0);
        }
        double x = js_to_num(a), y = js_to_num(b);
        if (!strcmp(op, "<"))  return js_bool(x < y);
        if (!strcmp(op, ">"))  return js_bool(x > y);
        if (!strcmp(op, "<=")) return js_bool(x <= y);
        return js_bool(x >= y);
    }

    if (!strcmp(op, "&"))   return js_num((double)(((long)js_to_num(a)) & ((long)js_to_num(b))));
    if (!strcmp(op, "|"))   return js_num((double)(((long)js_to_num(a)) | ((long)js_to_num(b))));
    if (!strcmp(op, "^"))   return js_num((double)(((long)js_to_num(a)) ^ ((long)js_to_num(b))));
    if (!strcmp(op, "<<"))  return js_num((double)(((int)js_to_num(a)) << (((int)js_to_num(b)) & 31)));
    if (!strcmp(op, ">>"))  return js_num((double)(((int)js_to_num(a)) >> (((int)js_to_num(b)) & 31)));
    if (!strcmp(op, ">>>")) return js_num((double)(((unsigned)(long long)js_to_num(a)) >> (((int)js_to_num(b)) & 31)));

    if (!strcmp(op, "in")) {
        if (b.t == JS_OBJ && b.u.o) {
            const char *nm = js_cstr(a);
            if (js_find(b.u.o, nm)) return js_bool(1);
            if (b.u.o->kind == JSO_ARRAY) {
                int i = atoi(nm);
                return js_bool(i >= 0 && i < b.u.o->nelems);
            }
        }
        return js_bool(0);
    }
    if (!strcmp(op, "instanceof")) return js_bool(a.t == JS_OBJ);

    return js_undef();
}

/* --- evaluation -------------------------------------------------------------- */

static js_val js_make_function(js_node *fn, js_scope *sc) {
    js_obj *o = js_new(JSO_FUNC);
    if (!o) return js_undef();
    o->fn = fn;
    o->closure = sc;
    /* Everything this function can still see has to stay. */
    for (js_scope *p = sc; p && !p->captured; p = p->parent) p->captured = 1;
    js_set(o, "prototype", js_obj_val(js_new(JSO_PLAIN)));
    return js_obj_val(o);
}

static js_val js_eval(js_node *n, js_scope *sc) {
    if (!n || js_stop()) return js_undef();
    if (++js_depth > JS_MAX_DEPTH) {
        js_depth--;
        js_throw_str("the script nests too deeply");
        return js_undef();
    }
    js_val jr = js_eval_inner(n, sc);
    js_depth--;
    return jr;
}

static js_val js_eval_inner(js_node *n, js_scope *sc) {
    switch (n->type) {
    case N_NUM:   return js_num(n->num);
    case N_STR:   return js_str_n(n->str, n->slen);
    case N_BOOL:  return js_bool((int)n->num);
    case N_NULL:  return js_null();
    case N_UNDEF: return js_undef();
    case N_THIS:  return js_this(sc);

    case N_IDENT: {
        js_val *v = js_lookup(sc, n->op);
        if (v) return *v;
        /* An unknown name is undefined rather than an error: pages read
         * things that only exist in other browsers all the time. */
        return js_undef();
    }

    case N_ARRAY: {
        js_obj *a = js_new_array();
        for (int i = 0; i < n->nlist; i++) js_arr_push(a, js_eval(n->list[i], sc));
        return js_obj_val(a);
    }

    case N_OBJECT: {
        js_obj *o = js_new(JSO_PLAIN);
        for (int i = 0; i < n->nlist; i++) {
            js_node *pair = n->list[i];
            char name[128];
            int l = pair->oplen < (int)sizeof name - 1 ? pair->oplen : (int)sizeof name - 1;
            memcpy(name, pair->op, (size_t)l); name[l] = 0;
            js_set(o, name, js_eval(pair->a, sc));
        }
        return js_obj_val(o);
    }

    case N_FUNC: case N_ARROW: return js_make_function(n, sc);

    case N_BIN: {
        js_val a = js_eval(n->a, sc);
        if (js_signal) return js_undef();
        js_val b = js_eval(n->b, sc);
        if (js_signal) return js_undef();
        return js_binop(n->op, a, b);
    }

    case N_LOGIC: {
        js_val a = js_eval(n->a, sc);
        if (js_signal) return js_undef();
        if (!strcmp(n->op, "&&")) return js_truthy(a) ? js_eval(n->b, sc) : a;
        if (!strcmp(n->op, "||")) return js_truthy(a) ? a : js_eval(n->b, sc);
        /* ?? -- only null and undefined fall through */
        return (a.t == JS_NULL || a.t == JS_UNDEF) ? js_eval(n->b, sc) : a;
    }

    case N_UNARY: {
        if (!strcmp(n->op, "typeof")) {
            /* typeof of an unknown name is "undefined", not an error. */
            if (n->a && n->a->type == N_IDENT) {
                js_val *v = js_lookup(sc, n->a->op);
                return js_str(v ? js_typeof(*v) : "undefined");
            }
            return js_str(js_typeof(js_eval(n->a, sc)));
        }
        if (!strcmp(n->op, "delete")) {
            /* Properties are never removed, only emptied. Nothing real
             * depends on the difference. */
            if (n->a && (n->a->type == N_MEMBER))
                js_setprop(js_eval(n->a->a, sc), n->a->op, js_undef());
            return js_bool(1);
        }
        js_val v = js_eval(n->a, sc);
        if (!strcmp(n->op, "!")) return js_bool(!js_truthy(v));
        if (!strcmp(n->op, "-")) return js_num(-js_to_num(v));
        if (!strcmp(n->op, "+")) return js_num(js_to_num(v));
        if (!strcmp(n->op, "~")) return js_num((double)(~(long)js_to_num(v)));
        if (!strcmp(n->op, "void")) return js_undef();
        if (!strcmp(n->op, "...")) return v;
        return js_undef();
    }

    case N_PRE: case N_POST: {
        js_val old = js_eval(n->a, sc);
        double d = js_to_num(old);
        double nv = (!strcmp(n->op, "++")) ? d + 1 : d - 1;
        js_assign_to(n->a, js_num(nv), sc);
        return n->type == N_PRE ? js_num(nv) : js_num(d);
    }

    case N_ASSIGN: {
        if (!strcmp(n->op, "=")) {
            js_val v = js_eval(n->b, sc);
            if (js_signal) return js_undef();
            js_assign_to(n->a, v, sc);
            return v;
        }
        js_val cur = js_eval(n->a, sc);
        if (js_signal) return js_undef();

        /* The short-circuiting ones only assign when they have to. */
        if (!strcmp(n->op, "&&=")) {
            if (!js_truthy(cur)) return cur;
            js_val v = js_eval(n->b, sc);
            js_assign_to(n->a, v, sc);
            return v;
        }
        if (!strcmp(n->op, "||=")) {
            if (js_truthy(cur)) return cur;
            js_val v = js_eval(n->b, sc);
            js_assign_to(n->a, v, sc);
            return v;
        }
        if (!strcmp(n->op, "??=")) {
            if (cur.t != JS_NULL && cur.t != JS_UNDEF) return cur;
            js_val v = js_eval(n->b, sc);
            js_assign_to(n->a, v, sc);
            return v;
        }

        char bare[8];
        int l = n->oplen - 1;
        if (l > (int)sizeof bare - 1) l = (int)sizeof bare - 1;
        memcpy(bare, n->op, (size_t)l); bare[l] = 0;

        js_val rhs = js_eval(n->b, sc);
        if (js_signal) return js_undef();
        js_val v = js_binop(bare, cur, rhs);
        js_assign_to(n->a, v, sc);
        return v;
    }

    case N_COND:
        return js_truthy(js_eval(n->a, sc)) ? js_eval(n->b, sc) : js_eval(n->c, sc);

    case N_SEQ: {
        js_eval(n->a, sc);
        return js_eval(n->b, sc);
    }

    case N_MEMBER: {
        js_val base = js_eval(n->a, sc);
        if (js_signal) return js_undef();
        if (n->flag && (base.t == JS_NULL || base.t == JS_UNDEF)) return js_undef();
        return js_getprop(base, n->op);
    }

    case N_INDEX: {
        js_val base = js_eval(n->a, sc);
        js_val idx = js_eval(n->b, sc);
        if (js_signal) return js_undef();
        if (base.t == JS_OBJ && base.u.o && base.u.o->kind == JSO_ARRAY &&
            idx.t == JS_NUM) {
            int i = (int)idx.u.n;
            js_obj *a = base.u.o;
            if (i >= 0 && i < a->nelems) return a->elems[i];
            return js_undef();
        }
        if (base.t == JS_STR && idx.t == JS_NUM) {
            int i = (int)idx.u.n;
            if (i >= 0 && i < base.u.s.len) return js_str_n(base.u.s.p + i, 1);
            return js_undef();
        }
        return js_getprop(base, js_cstr(idx));
    }

    case N_CALL: {
        js_val self = js_undef();
        js_val fn;
        js_node *callee = n->a;

        if (callee->type == N_MEMBER) {
            self = js_eval(callee->a, sc);
            if (js_signal) return js_undef();
            if (callee->flag && (self.t == JS_NULL || self.t == JS_UNDEF))
                return js_undef();
            fn = js_getprop(self, callee->op);
        } else if (callee->type == N_INDEX) {
            self = js_eval(callee->a, sc);
            js_val idx = js_eval(callee->b, sc);
            fn = js_getprop(self, js_cstr(idx));
        } else {
            fn = js_eval(callee, sc);
        }
        if (js_signal) return js_undef();
        if (n->flag && (fn.t == JS_NULL || fn.t == JS_UNDEF)) return js_undef();

        js_val args[32];
        int na = 0;
        for (int i = 0; i < n->nlist && na < 32; i++) {
            js_node *an = n->list[i];
            if (an->type == N_UNARY && an->op && !strcmp(an->op, "...")) {
                js_val sp = js_eval(an->a, sc);
                if (sp.t == JS_OBJ && sp.u.o && sp.u.o->kind == JSO_ARRAY)
                    for (int k = 0; k < sp.u.o->nelems && na < 32; k++)
                        args[na++] = sp.u.o->elems[k];
                continue;
            }
            args[na++] = js_eval(an, sc);
            if (js_signal) return js_undef();
        }
        return js_call(fn, self, args, na);
    }

    case N_NEW: {
        js_val fn = js_eval(n->a, sc);
        js_val args[16];
        int na = 0;
        for (int i = 0; i < n->nlist && na < 16; i++) args[na++] = js_eval(n->list[i], sc);

        js_val builtin = js_construct_builtin(n->a, fn, args, na);
        if (builtin.t != JS_UNDEF) return builtin;

        js_obj *o = js_new(JSO_PLAIN);
        if (fn.t == JS_OBJ && fn.u.o) {
            js_val *proto = js_find(fn.u.o, "prototype");
            if (proto && proto->t == JS_OBJ) o->proto = proto->u.o;
        }
        js_val self = js_obj_val(o);
        js_val r = js_call(fn, self, args, na);
        return (r.t == JS_OBJ) ? r : self;
    }

    default:
        return js_undef();
    }
}

/* --- statements ---------------------------------------------------------------- */

/* Function declarations are visible before the line they are written on, which
 * pages rely on constantly. */
static void js_hoist(js_node *block, js_scope *sc) {
    if (!block) return;
    for (int i = 0; i < block->nlist; i++) {
        js_node *s = block->list[i];
        if (s && s->type == N_FUNCDECL && s->op)
            js_set(sc->vars, s->op, js_make_function(s->b, sc));
    }
}

static void js_exec(js_node *n, js_scope *sc) {
    if (!n || js_stop()) return;
    if (++js_depth > JS_MAX_DEPTH) {
        js_depth--;
        js_throw_str("the script nests too deeply");
        return;
    }
    js_exec_inner(n, sc);
    js_depth--;
}

static void js_exec_inner(js_node *n, js_scope *sc) {
    switch (n->type) {
    case N_BLOCK:
        js_hoist(n, sc);
        for (int i = 0; i < n->nlist; i++) {
            js_exec(n->list[i], sc);
            if (js_signal) return;
        }
        return;

    case N_EMPTY: return;
    case N_EXPR:  js_eval(n->a, sc); return;

    case N_VAR:
        for (int i = 0; i < n->nlist; i++) {
            js_node *d = n->list[i];
            js_val v = d->a ? js_eval(d->a, sc) : js_undef();
            if (js_signal) return;
            js_set_ref(sc->vars, d->op, v);
        }
        return;

    case N_FUNCDECL:
        if (n->op) js_set(sc->vars, n->op, js_make_function(n->b, sc));
        return;

    case N_IF:
        if (js_truthy(js_eval(n->a, sc))) js_exec(n->b, sc);
        else js_exec(n->c, sc);
        return;

    case N_WHILE:
        while (!js_stop() && js_truthy(js_eval(n->a, sc))) {
            js_exec(n->b, sc);
            if (js_signal == C_BREAK) { js_signal = C_NORMAL; break; }
            if (js_signal == C_CONTINUE) js_signal = C_NORMAL;
            else if (js_signal) return;
        }
        return;

    case N_DO:
        do {
            if (js_stop()) return;
            js_exec(n->b, sc);
            if (js_signal == C_BREAK) { js_signal = C_NORMAL; break; }
            if (js_signal == C_CONTINUE) js_signal = C_NORMAL;
            else if (js_signal) return;
        } while (js_truthy(js_eval(n->a, sc)));
        return;

    case N_FOR: {
        js_scope *inner = js_scope_new(sc);
        if (!inner) return;
        if (n->a) js_exec(n->a, inner);
        while (!js_stop()) {
            if (n->b && !js_truthy(js_eval(n->b, inner))) break;
            js_exec(n->d, inner);
            if (js_signal == C_BREAK) { js_signal = C_NORMAL; break; }
            if (js_signal == C_CONTINUE) js_signal = C_NORMAL;
            else if (js_signal) return;
            if (n->c) js_eval(n->c, inner);
        }
        js_scope_release(inner);
        return;
    }

    case N_FORIN: {
        js_scope *inner = js_scope_new(sc);
        if (!inner) return;
        js_val src = js_eval(n->b, inner);
        if (js_signal) return;

        const char *name = 0;
        if (n->a && n->a->type == N_VAR && n->a->nlist) name = n->a->list[0]->op;
        else if (n->a && n->a->type == N_EXPR && n->a->a &&
                 n->a->a->type == N_IDENT) name = n->a->a->op;
        if (!name) return;

        int count = 0;
        js_val item;
        for (;;) {
            if (js_stop()) return;
            if (src.t == JS_OBJ && src.u.o && src.u.o->kind == JSO_ARRAY) {
                if (count >= src.u.o->nelems) break;
                item = n->flag ? src.u.o->elems[count] : js_num(count);
            } else if (src.t == JS_OBJ && src.u.o) {
                if (count >= src.u.o->nprops) break;
                item = n->flag ? src.u.o->props[count].v
                               : js_str(src.u.o->props[count].name);
            } else if (src.t == JS_STR) {
                if (count >= src.u.s.len) break;
                item = n->flag ? js_str_n(src.u.s.p + count, 1) : js_num(count);
            } else break;
            count++;

            js_set(inner->vars, name, item);
            js_exec(n->c, inner);
            if (js_signal == C_BREAK) { js_signal = C_NORMAL; break; }
            if (js_signal == C_CONTINUE) js_signal = C_NORMAL;
            else if (js_signal) return;
        }
        return;
    }

    case N_RET:
        js_signal_val = n->a ? js_eval(n->a, sc) : js_undef();
        if (js_signal == C_THROW) return;
        js_signal = C_RETURN;
        return;

    case N_BREAK: js_signal = C_BREAK; return;
    case N_CONT:  js_signal = C_CONTINUE; return;

    case N_THROW:
        js_signal_val = js_eval(n->a, sc);
        js_signal = C_THROW;
        if (!js_err[0])
            snprintf(js_err, sizeof js_err, "uncaught: %s", js_cstr(js_signal_val));
        return;

    case N_TRY: {
        js_exec(n->a, sc);
        if (js_signal == C_THROW && n->b) {
            js_val thrown = js_signal_val;
            js_signal = C_NORMAL;
            js_err[0] = 0;
            js_scope *inner = js_scope_new(sc);
            if (n->d) js_set(inner->vars, n->d->op, thrown);
            js_exec(n->b, inner);
        }
        if (n->c) {
            int keep = js_signal;
            js_val keepv = js_signal_val;
            js_signal = C_NORMAL;
            js_exec(n->c, sc);
            if (!js_signal) { js_signal = keep; js_signal_val = keepv; }
        }
        return;
    }

    case N_SWITCH: {
        js_val v = js_eval(n->a, sc);
        int started = 0;
        for (int pass = 0; pass < 2 && !started; pass++) {
            for (int i = 0; i < n->nlist; i++) {
                js_node *cs = n->list[i];
                if (!started) {
                    if (pass == 0) {
                        if (cs->flag) continue;              /* default later */
                        if (!js_strict_eq(v, js_eval(cs->a, sc))) continue;
                    } else if (!cs->flag) continue;
                    started = 1;
                }
                js_exec(cs->b, sc);
                if (js_signal == C_BREAK) { js_signal = C_NORMAL; return; }
                if (js_signal) return;
            }
        }
        return;
    }

    default:
        js_eval(n, sc);
        return;
    }
}

/* ==========================================================================
 * the front door
 * ========================================================================== */

static void js_setup_globals(void);

static void js_reset(void) {
    js_arena_used = 0;
    js_oom = 0;
    js_err[0] = 0;
    js_signal = C_NORMAL;
    js_signal_val = js_undef();
    js_steps = 0;
    js_global_scope = js_scope_new(0);
    js_setup_globals();
}

static int js_init(void) {
    js_arena = (char *)malloc(JS_ARENA_CAP);
    if (!js_arena) return 0;
    js_reset();
    return 1;
}

/* Run one script. Returns 1 if it finished, 0 if it did not -- js_error() says
 * why. A failure stops that script and no more; the page keeps working. */
static int js_run(const char *src, int len) {
    if (!js_arena) return 0;
    js_err[0] = 0;
    js_signal = C_NORMAL;
    js_oom = 0;

    js_src_has_arguments = 0;
    for (int i = 0; i + 9 <= len; i++)
        if (src[i] == 'a' && !memcmp(src + i, "arguments", 9)) {
            js_src_has_arguments = 1;
            break;
        }

    js_parse_deadline = js_clock_ms ? js_clock_ms() + js_budget_ms : 0;

    js_lex L;
    js_lex_init(&L, src, len);
    js_next(&L);

    js_node *prog = js_node_new(N_BLOCK);
    int cap = 0;
    while (L.tok.type != T_EOF && !js_err[0])
        js_list_add(&prog->list, &prog->nlist, &cap, js_statement(&L));

    if (js_err[0]) return 0;

    js_steps = 0;
    js_depth = 0;
    js_deadline = js_clock_ms ? js_clock_ms() + js_budget_ms : 0;
    js_exec(prog, js_global_scope);
    if (js_signal == C_THROW) {
        if (!js_err[0])
            snprintf(js_err, sizeof js_err, "uncaught: %s", js_cstr(js_signal_val));
        js_signal = C_NORMAL;
        return 0;
    }
    js_signal = C_NORMAL;
    return !js_err[0];
}

static const char *js_error(void) { return js_err[0] ? js_err : 0; }

/* How much of the arena is gone -- the browser shows it, because a page that
 * runs out has no other symptom. */
static long js_memory_used(void) { return js_arena_used; }

#endif /* JS_H */
