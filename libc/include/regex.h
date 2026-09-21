/* POSIX regular expressions.
 *
 * Both dialects are here: extended (REG_EXTENDED), where `(`, `|`, `+`, `?`
 * and `{` are operators, and basic, where those are ordinary characters and
 * `\(`, `\{` are the operators instead. Bracket expressions understand
 * ranges, negation and the [:name:] classes. There are no back references in
 * either dialect -- POSIX does not define them for extended expressions, and
 * a basic expression that uses \1 is refused rather than quietly mismatched.
 *
 * One deliberate difference from the standard, stated because it is
 * invisible from the outside: where POSIX asks for the longest match among
 * the alternatives, this takes the first that matches, left to right, the
 * way Perl and every backtracking engine does. The two differ only when an
 * earlier alternative can match a shorter piece of the same text than a
 * later one could -- write the longer alternative first and they agree.
 */
#ifndef REGEX_H
#define REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long regoff_t;

typedef struct {
    size_t re_nsub;      /* how many parenthesised groups there are */
    void  *re_prog;      /* the compiled form; regfree() releases it */
    int    re_flags;
} regex_t;

typedef struct {
    regoff_t rm_so;      /* where it starts, or -1 for a group that did not
                            take part in the match */
    regoff_t rm_eo;      /* one past where it ends */
} regmatch_t;

/* regcomp flags */
#define REG_EXTENDED 0x01   /* the extended dialect */
#define REG_ICASE    0x02   /* upper and lower case are the same letter */
#define REG_NOSUB    0x04   /* report only whether it matched */
#define REG_NEWLINE  0x08   /* `.` and [^...] stop at a newline, and ^ and $
                               match beside one */

/* regexec flags */
#define REG_NOTBOL   0x10   /* the string does not begin a line */
#define REG_NOTEOL   0x20   /* the string does not end one */

/* Return values. REG_NOMATCH is 1 so that a plain truth test on regexec
 * reads as "did not match", as it does everywhere else. */
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string,
               size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf,
                size_t errbuf_size);
void   regfree(regex_t *preg);

#ifdef __cplusplus
}
#endif

#endif
