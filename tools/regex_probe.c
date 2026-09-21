/* One program, built twice: once against xyuOS's regex.h and regex.c, once
 * against the host's. Feeding both the same cases and diffing the output is
 * the only test of a regex engine worth having -- a table of expected
 * answers is just the author's opinion written down twice.
 *
 * Reads cases from standard input, one per line, tab separated:
 *
 *     <flags>\t<pattern>\t<subject>
 *
 * where <flags> is any of e (extended), i (ignore case), n (newline),
 * s (nosub), or "-" for none. In the pattern and the subject, \t \n \r \f
 * and \\ stand for themselves.
 */
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void unescape(const char *in, char *out) {
    while (*in) {
        if (*in == '\\' && in[1]) {
            in++;
            switch (*in++) {
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case 'r': *out++ = '\r'; break;
            case 'f': *out++ = '\f'; break;
            case '\\': *out++ = '\\'; break;
            default: *out++ = in[-1]; break;
            }
        } else {
            *out++ = *in++;
        }
    }
    *out = 0;
}

/* Big enough for the long-subject cases, which exist to show that matching
 * does not put a stack frame on per character. A short buffer here would cut
 * them into several lines and quietly turn them into different cases. */
#define BIG (1 << 20)

static char line[BIG], pat[BIG], subj[BIG];

int main(void) {
    int caseno = 0;

    while (fgets(line, BIG, stdin)) {
        size_t n = strlen(line);
        if (n == BIG - 1) { printf("%d LINE-TOO-LONG\n", ++caseno); continue; }
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0 || line[0] == '#') continue;
        caseno++;

        char *f = line;
        char *p = strchr(f, '\t');
        if (!p) { printf("%d BADCASE\n", caseno); continue; }
        *p++ = 0;
        char *s = strchr(p, '\t');
        if (!s) { printf("%d BADCASE\n", caseno); continue; }
        *s++ = 0;

        int cflags = 0;
        for (char *c = f; *c; c++) {
            if (*c == 'e') cflags |= REG_EXTENDED;
            if (*c == 'i') cflags |= REG_ICASE;
            if (*c == 'n') cflags |= REG_NEWLINE;
            if (*c == 's') cflags |= REG_NOSUB;
        }

        unescape(p, pat);
        unescape(s, subj);

        regex_t re;
        int rc = regcomp(&re, pat, cflags);
        if (rc != 0) {
            /* The numeric codes differ between implementations by design;
             * what matters is that both refuse the same patterns. */
            /* The reason goes to stderr, not stdout: the two libraries word
             * their messages differently and comparing the wording would
             * report a difference that is not one. */
            char eb[128];
            regerror(rc, &re, eb, sizeof eb);
            fprintf(stderr, "case %d refused: %s\n", caseno, eb);
            printf("%d COMPILE-FAILED\n", caseno);
            continue;
        }

        regmatch_t m[12];
        for (int i = 0; i < 12; i++) m[i].rm_so = m[i].rm_eo = -1;

        rc = regexec(&re, subj, 12, m, 0);
        if (rc != 0) {
            printf("%d NOMATCH\n", caseno);
        } else {
            printf("%d MATCH nsub=%d", caseno, (int)re.re_nsub);
            for (size_t i = 0; i <= re.re_nsub && i < 12; i++)
                printf(" [%ld,%ld]", (long)m[i].rm_so, (long)m[i].rm_eo);
            printf("\n");
        }
        regfree(&re);
    }
    return 0;
}
