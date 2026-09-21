/* POSIX regular expressions: a parser into a tree, and a backtracking
 * matcher over it. See regex.h for what is and is not promised.
 *
 * The matcher is written in continuation-passing style, which is what makes
 * the tree form workable: every node is asked to match at a position and
 * then to hand the rest of the pattern the position it reached. Without that
 * a concatenation has no way to tell its left half how much to take, and
 * `(ab|a)b` against "ab" fails -- the classic wrong answer. Here the left
 * half tries `ab`, the continuation fails on the trailing `b`, and control
 * comes back to try `a` instead.
 *
 * Repetition carries the position its current round began at. A body that
 * can match nothing -- `(a*)*` -- would otherwise loop for ever; when a
 * round consumes nothing, the repetition stops rather than going round again
 * to consume nothing a second time.
 */

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* --- the tree ----------------------------------------------------------- */

enum {
    N_CHAR,    /* one byte */
    N_ANY,     /* . */
    N_SET,     /* [...] */
    N_BOL,     /* ^ */
    N_EOL,     /* $ */
    N_CAT,     /* a then b */
    N_ALT,     /* a or b */
    N_REP,     /* a, between min and max times */
    N_GROUP,   /* ( a ), remembering where it fell */
    N_GEND,    /* the closing half of a group; never parsed, only linked to */
    N_EMPTY    /* matches here, consuming nothing */
};

typedef struct node node;
struct node {
    unsigned char type;
    unsigned char ch;          /* N_CHAR */
    unsigned char set[32];     /* N_SET, one bit per byte value */
    int   gno;                 /* N_GROUP, N_GEND */
    int   min, max;            /* N_REP; max < 0 means no limit */
    node *a, *b;               /* children */
    node *end;                 /* N_GROUP -> its own N_GEND */
};

typedef struct {
    node  *arena;              /* every node, in one block */
    int    used, cap;
    int    ngroup;
    int    flags;
    node  *root;
} prog;

/* --- parsing ------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *start;         /* the whole pattern, for `^` in the basic
                                  dialect, where it anchors only here */
    prog       *pr;
    int         err;
    int         ext;           /* extended dialect */
} parser;

static node *alt(parser *ps);

static node *nnew(parser *ps, int type) {
    if (ps->pr->used >= ps->pr->cap) { ps->err = REG_ESPACE; return NULL; }
    node *n = &ps->pr->arena[ps->pr->used++];
    memset(n, 0, sizeof *n);
    n->type = (unsigned char)type;
    n->max = -1;
    return n;
}

static void set_add(node *n, unsigned c) { n->set[c >> 3] |= 1u << (c & 7); }
static int  set_has(const node *n, unsigned c) {
    return (n->set[c >> 3] >> (c & 7)) & 1;
}

/* In the extended dialect these are operators bare; in the basic one they
 * are operators only behind a backslash. `at` points at the character after
 * any backslash. */
static int is_op(parser *ps, const char *p, char want) {
    if (ps->ext) return *p == want;
    return p[0] == '\\' && p[1] == want;
}
static const char *skip_op(parser *ps, const char *p) {
    return p + (ps->ext ? 1 : 2);
}

static const struct { const char *name; int (*fn)(int); } classes[] = {
    { "alpha",  isalpha  }, { "digit",  isdigit  }, { "alnum",  isalnum  },
    { "space",  isspace  }, { "upper",  isupper  }, { "lower",  islower  },
    { "punct",  ispunct  }, { "print",  isprint  }, { "graph",  isgraph  },
    { "cntrl",  iscntrl  }, { "xdigit", isxdigit }, { "blank",  NULL     },
    { NULL, NULL }
};

static int is_blank(int c) { return c == ' ' || c == '\t'; }

/* One backslash escape, outside a bracket or inside one. Returns the byte,
 * or -1 if the backslash ends the pattern. */
static int escaped(parser *ps, const char **pp) {
    const char *p = *pp + 1;
    if (*p == 0) { ps->err = REG_EESCAPE; return -1; }
    *pp = p + 1;
    switch (*p) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    default:  return (unsigned char)*p;   /* the character itself */
    }
}

static node *bracket(parser *ps) {
    node *n = nnew(ps, N_SET);
    if (!n) return NULL;

    const char *p = ps->p + 1;             /* past the '[' */
    int negate = 0;
    if (*p == '^') { negate = 1; p++; }

    int first = 1;
    for (;;) {
        if (*p == 0) { ps->err = REG_EBRACK; return NULL; }
        if (*p == ']' && !first) break;
        first = 0;

        /* [:alpha:] and friends. */
        if (p[0] == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (!e) { ps->err = REG_ECTYPE; return NULL; }
            size_t len = (size_t)(e - (p + 2));
            int i, found = 0;
            for (i = 0; classes[i].name; i++) {
                if (strlen(classes[i].name) == len &&
                    memcmp(classes[i].name, p + 2, len) == 0) {
                    int (*fn)(int) = classes[i].fn ? classes[i].fn : is_blank;
                    for (unsigned c = 0; c < 256; c++)
                        if (fn((int)c)) set_add(n, c);
                    found = 1;
                    break;
                }
            }
            if (!found) { ps->err = REG_ECTYPE; return NULL; }
            p = e + 2;
            continue;
        }

        /* A backslash in here is a backslash. POSIX gives it no meaning
         * inside a bracket expression, and CSS's own @import rule leans on
         * that: it writes [\] for the set holding one backslash. Reading it
         * as an escape swallows the closing bracket and the expression runs
         * off into the rest of the pattern. */
        int lo = (unsigned char)*p++;

        /* A '-' before the closing bracket is itself. */
        if (p[0] == '-' && p[1] != ']' && p[1] != 0) {
            p++;
            int hi = (unsigned char)*p++;
            if (hi < lo) { ps->err = REG_ERANGE; return NULL; }
            for (int c = lo; c <= hi; c++) set_add(n, (unsigned)c);
        } else {
            set_add(n, (unsigned)lo);
        }
    }
    ps->p = p + 1;                          /* past the ']' */

    if (ps->pr->flags & REG_ICASE)
        for (unsigned c = 'a'; c <= 'z'; c++) {
            if (set_has(n, c)) set_add(n, c - 32);
            if (set_has(n, c - 32)) set_add(n, c);
        }

    if (negate) {
        for (int i = 0; i < 32; i++) n->set[i] = (unsigned char)~n->set[i];
        /* A negated set does not cross a line when asked not to. */
        if (ps->pr->flags & REG_NEWLINE) n->set['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
    }
    return n;
}

static node *atom(parser *ps) {
    const char *p = ps->p;

    if (is_op(ps, p, '(')) {
        ps->p = skip_op(ps, p);
        node *g = nnew(ps, N_GROUP);
        if (!g) return NULL;
        g->gno = ++ps->pr->ngroup;
        g->a = alt(ps);
        if (ps->err) return NULL;
        if (!is_op(ps, ps->p, ')')) { ps->err = REG_EPAREN; return NULL; }
        ps->p = skip_op(ps, ps->p);
        g->end = nnew(ps, N_GEND);
        if (!g->end) return NULL;
        g->end->gno = g->gno;
        return g;
    }

    if (*p == '[') return bracket(ps);

    if (*p == '.') {
        ps->p = p + 1;
        return nnew(ps, N_ANY);
    }

    /* ^ and $ anchor only where they can: in the basic dialect they are
     * ordinary characters anywhere else. */
    if (*p == '^' && (ps->ext || p == ps->start)) {
        ps->p = p + 1;
        return nnew(ps, N_BOL);
    }
    if (*p == '$' && (ps->ext || p[1] == 0)) {
        ps->p = p + 1;
        return nnew(ps, N_EOL);
    }

    int c;
    if (*p == '\\') {
        if (!ps->ext && p[1] >= '1' && p[1] <= '9') {
            ps->err = REG_ESUBREG;          /* no back references here */
            return NULL;
        }
        c = escaped(ps, &p);
        if (c < 0) return NULL;
        ps->p = p;
    } else {
        if (*p == 0) return nnew(ps, N_EMPTY);
        c = (unsigned char)*p;
        ps->p = p + 1;
    }

    node *n = nnew(ps, N_CHAR);
    if (!n) return NULL;
    n->ch = (unsigned char)((ps->pr->flags & REG_ICASE) ? tolower(c) : c);
    return n;
}

/* {m}, {m,}, {m,n} */
static int interval(parser *ps, int *min, int *max) {
    const char *p = skip_op(ps, ps->p);
    if (!isdigit((unsigned char)*p)) { ps->err = REG_BADBR; return 0; }

    int lo = 0;
    while (isdigit((unsigned char)*p)) lo = lo * 10 + (*p++ - '0');
    int hi = lo;
    if (*p == ',') {
        p++;
        if (isdigit((unsigned char)*p)) {
            hi = 0;
            while (isdigit((unsigned char)*p)) hi = hi * 10 + (*p++ - '0');
        } else {
            hi = -1;
        }
    }
    if (!is_op(ps, p, '}')) { ps->err = REG_EBRACE; return 0; }
    if (hi >= 0 && hi < lo)  { ps->err = REG_BADBR;  return 0; }

    ps->p = skip_op(ps, p);
    *min = lo; *max = hi;
    return 1;
}

static node *piece(parser *ps) {
    node *a = atom(ps);
    if (!a || ps->err) return NULL;

    for (;;) {
        int min, max;
        if (*ps->p == '*') { min = 0; max = -1; ps->p++; }
        else if (ps->ext && *ps->p == '+') { min = 1; max = -1; ps->p++; }
        else if (ps->ext && *ps->p == '?') { min = 0; max =  1; ps->p++; }
        /* A brace that does not open a count is an ordinary character, so
         * it has to be looked past before it is treated as an operator. */
        else if (is_op(ps, ps->p, '{') &&
                 isdigit((unsigned char)*skip_op(ps, ps->p))) {
            if (!interval(ps, &min, &max)) return NULL;
        } else break;

        node *r = nnew(ps, N_REP);
        if (!r) return NULL;
        r->a = a; r->min = min; r->max = max;
        a = r;
    }
    return a;
}

static int at_end(parser *ps) {
    if (*ps->p == 0) return 1;
    if (is_op(ps, ps->p, '|')) return 1;
    if (is_op(ps, ps->p, ')')) return 1;
    return 0;
}

static node *seq(parser *ps) {
    if (at_end(ps)) return nnew(ps, N_EMPTY);

    node *left = piece(ps);
    if (!left || ps->err) return NULL;

    while (!at_end(ps)) {
        node *right = piece(ps);
        if (!right || ps->err) return NULL;
        node *c = nnew(ps, N_CAT);
        if (!c) return NULL;
        c->a = left; c->b = right;
        left = c;
    }
    return left;
}

static node *alt(parser *ps) {
    node *left = seq(ps);
    if (!left || ps->err) return NULL;

    while (is_op(ps, ps->p, '|')) {
        ps->p = skip_op(ps, ps->p);
        node *right = seq(ps);
        if (!right || ps->err) return NULL;
        node *a = nnew(ps, N_ALT);
        if (!a) return NULL;
        a->a = left; a->b = right;
        left = a;
    }
    return left;
}

/* --- matching ----------------------------------------------------------- */

enum { K_RUN, K_REP };

typedef struct cont {
    int          kind;
    node        *n;
    int          count;        /* K_REP: rounds finished */
    const char  *from;         /* K_REP: where this round began */
    struct cont *next;
} cont;

typedef struct {
    const char *begin, *end;
    int         flags;         /* compile flags */
    int         eflags;
    regmatch_t *m;             /* one per group, plus [0] */
    const char **gs;           /* where each open group started */
    int         ngroup;
    int         nosub;
} rctx;

static const char *m_node(rctx *c, node *n, const char *s, cont *k);

static int same(rctx *c, int a, int b);

/* Whether a node that eats exactly one byte accepts the one at `s`. Pulled
 * out because the repetition below counts those bytes in a loop rather than
 * recursing per byte -- `[^"]*` across a stylesheet would otherwise put one
 * stack frame on for every character in the file. */
static int one_byte(rctx *c, node *n, const char *s) {
    if (s >= c->end) return 0;
    switch (n->type) {
    case N_CHAR: return same(c, (unsigned char)*s, n->ch);
    case N_ANY:  return !((c->flags & REG_NEWLINE) && *s == '\n');
    case N_SET:  return set_has(n, (unsigned char)*s);
    default:     return 0;
    }
}

static int eats_one_byte(const node *n) {
    return n->type == N_CHAR || n->type == N_ANY || n->type == N_SET;
}

static const char *run(rctx *c, cont *k, const char *s) {
    if (!k) return s;                       /* the whole pattern matched */
    if (k->kind == K_RUN) return m_node(c, k->n, s, k->next);

    node *r = k->n;
    /* A round that consumed nothing will consume nothing next time either.
     * Where the count has already been met, the round is refused rather than
     * merely stopped at: refusing it unwinds whatever its body recorded, so
     * `(a*)*` against "aaa" reports the group as the "aaa" it really took
     * and not the empty match that followed it. Where rounds are still owed,
     * an empty body satisfies them all at once and there is nothing to
     * unwind. */
    if (k->count > 0 && s == k->from)
        return (k->count > r->min) ? NULL : run(c, k->next, s);

    if (k->count < r->min) {
        cont kk = { K_REP, r, k->count + 1, s, k->next };
        return m_node(c, r->a, s, &kk);
    }
    if (r->max < 0 || k->count < r->max) {
        cont kk = { K_REP, r, k->count + 1, s, k->next };
        const char *got = m_node(c, r->a, s, &kk);
        if (got) return got;                /* greedy: more rounds first */
    }
    return run(c, k->next, s);
}

static int same(rctx *c, int a, int b) {
    if (c->flags & REG_ICASE) return tolower(a) == tolower(b);
    return a == b;
}

static const char *m_node(rctx *c, node *n, const char *s, cont *k) {
    switch (n->type) {

    case N_EMPTY:
        return run(c, k, s);

    case N_CHAR:
        if (s < c->end && same(c, (unsigned char)*s, n->ch))
            return run(c, k, s + 1);
        return NULL;

    case N_ANY:
        if (s < c->end && !((c->flags & REG_NEWLINE) && *s == '\n'))
            return run(c, k, s + 1);
        return NULL;

    case N_SET:
        if (s < c->end && set_has(n, (unsigned char)*s))
            return run(c, k, s + 1);
        return NULL;

    case N_BOL:
        if (s == c->begin) {
            if (c->eflags & REG_NOTBOL) return NULL;
            return run(c, k, s);
        }
        if ((c->flags & REG_NEWLINE) && s[-1] == '\n') return run(c, k, s);
        return NULL;

    case N_EOL:
        if (s == c->end) {
            if (c->eflags & REG_NOTEOL) return NULL;
            return run(c, k, s);
        }
        if ((c->flags & REG_NEWLINE) && *s == '\n') return run(c, k, s);
        return NULL;

    case N_CAT: {
        cont kk = { K_RUN, n->b, 0, NULL, k };
        return m_node(c, n->a, s, &kk);
    }

    case N_ALT: {
        const char *got = m_node(c, n->a, s, k);
        return got ? got : m_node(c, n->b, s, k);
    }

    case N_REP: {
        /* When the body is one byte wide, take as many as there are and
         * then give ground a byte at a time. Same answer as the general
         * case, in constant stack. */
        if (eats_one_byte(n->a)) {
            const char *p = s;
            int count = 0;
            while ((n->max < 0 || count < n->max) && one_byte(c, n->a, p)) {
                p++; count++;
            }
            while (count >= n->min) {
                const char *got = run(c, k, p);
                if (got) return got;
                if (count == 0) break;
                p--; count--;
            }
            return NULL;
        }
        cont kk = { K_REP, n, 0, s, k };
        return run(c, &kk, s);
    }

    case N_GROUP: {
        if (c->nosub) return m_node(c, n->a, s, k);   /* nothing to remember */
        const char *was = c->gs[n->gno];
        c->gs[n->gno] = s;
        cont kk = { K_RUN, n->end, 0, NULL, k };
        const char *got = m_node(c, n->a, s, &kk);
        if (!got) c->gs[n->gno] = was;      /* this attempt did not stand */
        return got;
    }

    case N_GEND: {
        regmatch_t was = c->m[n->gno];
        c->m[n->gno].rm_so = c->gs[n->gno] - c->begin;
        c->m[n->gno].rm_eo = s - c->begin;
        const char *got = run(c, k, s);
        if (!got) c->m[n->gno] = was;
        return got;
    }
    }
    return NULL;
}

/* --- the interface ------------------------------------------------------ */

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;

    size_t len = strlen(pattern);
    /* Every character of the pattern makes at most one node, and the
     * combining nodes -- concatenation, alternation, repetition, the closing
     * half of a group -- add at most one more apiece. */
    int cap = (int)(2 * len + 8);

    prog *pr = calloc(1, sizeof *pr);
    if (!pr) return REG_ESPACE;
    pr->arena = calloc((size_t)cap, sizeof(node));
    if (!pr->arena) { free(pr); return REG_ESPACE; }
    pr->cap = cap;
    pr->flags = cflags;

    parser ps;
    ps.p = pattern;
    ps.start = pattern;
    ps.pr = pr;
    ps.err = 0;
    ps.ext = (cflags & REG_EXTENDED) != 0;

    pr->root = alt(&ps);
    if (!ps.err && *ps.p != 0)
        ps.err = is_op(&ps, ps.p, ')') ? REG_EPAREN : REG_BADPAT;

    if (ps.err) {
        free(pr->arena);
        free(pr);
        preg->re_prog = NULL;
        preg->re_nsub = 0;
        return ps.err;
    }

    preg->re_prog = pr;
    preg->re_nsub = (size_t)pr->ngroup;
    preg->re_flags = cflags;
    return 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags) {
    if (!preg || !preg->re_prog || !string) return REG_BADPAT;
    prog *pr = (prog *)preg->re_prog;

    int ng = pr->ngroup;
    int nosub = (pr->flags & REG_NOSUB) != 0 || nmatch == 0 || pmatch == NULL;

    regmatch_t *m = malloc(sizeof(regmatch_t) * (size_t)(ng + 1));
    const char **gs = malloc(sizeof(char *) * (size_t)(ng + 1));
    if (!m || !gs) { free(m); free(gs); return REG_ESPACE; }

    rctx c;
    c.begin  = string;
    c.end    = string + strlen(string);
    c.flags  = pr->flags;
    c.eflags = eflags;
    c.m      = m;
    c.gs     = gs;
    c.ngroup = ng;
    c.nosub  = nosub;

    /* Leftmost: the earliest starting point that matches at all wins. */
    for (const char *at = string; at <= c.end; at++) {
        for (int i = 0; i <= ng; i++) {
            m[i].rm_so = m[i].rm_eo = -1;
            gs[i] = NULL;
        }
        const char *got = m_node(&c, pr->root, at, NULL);
        if (!got) continue;

        if (!nosub) {
            m[0].rm_so = at - string;
            m[0].rm_eo = got - string;
            for (size_t i = 0; i < nmatch; i++)
                pmatch[i] = (i <= (size_t)ng) ? m[i]
                                              : (regmatch_t){ -1, -1 };
        }
        free(m); free(gs);
        return 0;
    }

    free(m); free(gs);
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf,
                size_t errbuf_size) {
    (void)preg;
    const char *msg;
    switch (errcode) {
    case 0:            msg = "no error";                          break;
    case REG_NOMATCH:  msg = "no match";                          break;
    case REG_BADPAT:   msg = "invalid regular expression";        break;
    case REG_ECOLLATE: msg = "invalid collating element";         break;
    case REG_ECTYPE:   msg = "unknown character class name";      break;
    case REG_EESCAPE:  msg = "trailing backslash";                break;
    case REG_ESUBREG:  msg = "back references are not supported"; break;
    case REG_EBRACK:   msg = "unmatched [";                       break;
    case REG_EPAREN:   msg = "unmatched (";                       break;
    case REG_EBRACE:   msg = "unmatched {";                       break;
    case REG_BADBR:    msg = "invalid repetition count";          break;
    case REG_ERANGE:   msg = "invalid range in a bracket";        break;
    case REG_ESPACE:   msg = "out of memory";                     break;
    case REG_BADRPT:   msg = "nothing to repeat";                 break;
    default:           msg = "unknown error";                     break;
    }

    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size > 0) {
        size_t take = (len < errbuf_size) ? len : errbuf_size;
        memcpy(errbuf, msg, take - 1);
        errbuf[take - 1] = 0;
    }
    return len;
}

void regfree(regex_t *preg) {
    if (!preg || !preg->re_prog) return;
    prog *pr = (prog *)preg->re_prog;
    free(pr->arena);
    free(pr);
    preg->re_prog = NULL;
    preg->re_nsub = 0;
}
