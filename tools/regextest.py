#!/usr/bin/env python3
"""Check xyuOS's regex against the host's, case by case.

The engine in libc/src/regex.c is a backtracking one, and the standard asks
for the longest match among alternatives rather than the first. Where the two
disagree that is a real difference and it is printed as one, not hidden: the
point of this is to know exactly where the difference lies, and to catch
everything else.
"""
import os, subprocess, sys

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

HOME = XYUOS
OUT = "/tmp/regexcheck"
os.makedirs(OUT, exist_ok=True)

# NetSurf's @import rule, the pattern this engine was written for. Assembled
# the way its C source assembles it, so the two cannot drift apart silently.
NS_IMPORT = (
    "@import"
    "[ \t\r\n\f]*"
    "("
    "\"(([^\"]|[\\]\")*)\""
    "|"
    "'(([^']|[\\]')*)'"
    "|"
    "url\\([ \t\r\n\f]*"
    "\"(([^\"]|[\\]\")*)\""
    "[ \t\r\n\f]*\\)"
    "|"
    "url\\([ \t\r\n\f]*"
    "'(([^']|[\\]')*)'"
    "[ \t\r\n\f]*\\)"
    "|"
    "url\\([ \t\r\n\f]*"
    "([^) \t\r\n\f]*)"
    "[ \t\r\n\f]*\\)"
    ")")


def esc(s):
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", "\\r").replace("\f", "\\f"))


CASES = []
KNOWN = {}       # case index -> why it is expected to differ


def case(flags, pattern, subject, known=None):
    if known:
        KNOWN[len(CASES)] = known
    CASES.append((flags, pattern, subject))


# The one difference this engine promises, spelt out so that it stays the
# only one. POSIX asks for the longest match among the alternatives; a
# backtracking engine takes the first that works, and stops looking.
FIRST_NOT_LONGEST = ("the first alternative that matches is taken, "
                     "where POSIX asks for the longest")


# --- the pattern this was written for -------------------------------------
for subj in [
    '@import "theme.css";',
    "@import 'theme.css';",
    '@import url("theme.css");',
    "@import url('theme.css');",
    '@import url(theme.css);',
    '@import   url(  theme.css  );',
    '@IMPORT URL("Theme.css");',
    '@import\t\r\n "a/b/c.css" ;',
    'body { color: red } @import "late.css";',
    '@import',
    '@import url(;',
    '@import "unterminated;',
    '@import url();',
]:
    case("ei", NS_IMPORT, subj)

# A quote escaped inside the string: the two alternatives [^"] and [\]" can
# both match at the backslash, and taking the shorter one ends the string
# early. This is the documented difference, in the wild.
case("ei", NS_IMPORT, '@import "a\\"b.css";', known=FIRST_NOT_LONGEST)
case("e", "(a|ab)", "ab", known=FIRST_NOT_LONGEST)
case("e", "(a|ab)c", "abc")          # with a continuation, both agree

# --- the parts of the syntax, one at a time -------------------------------
BASICS = [
    ("e", "a", "a"), ("e", "a", "b"), ("e", "a", ""),
    ("e", "abc", "xxabcxx"), ("e", "^abc", "xxabcxx"), ("e", "^abc", "abcxx"),
    ("e", "abc$", "xxabc"), ("e", "abc$", "xxabcx"),
    ("e", "^$", ""), ("e", "^$", "x"),
    ("e", "a*", ""), ("e", "a*", "aaa"), ("e", "a*b", "aaab"),
    ("e", "a+", "aaa"), ("e", "a+", "b"),
    ("e", "a?b", "ab"), ("e", "a?b", "b"),
    ("e", "a{2}", "aaa"), ("e", "a{2,}", "aaaa"), ("e", "a{2,3}", "aaaa"),
    ("e", "a{0}b", "b"), ("e", "a{4}", "aaa"),
    ("e", ".", "x"), ("e", ".", ""), ("e", "a.c", "abc"), ("e", "a.c", "ac"),
    ("e", "a|b", "b"), ("e", "a|b|c", "c"), ("e", "(a|b)c", "bc"),
    ("e", "(ab|a)b", "ab"),          # the classic backtracking case
    ("e", "(a|ab)(c|bcd)", "abcd"),
    ("e", "(a*)*", "aaa"),           # a body that can match nothing
    ("e", "(a*)+", "b"),
    ("e", "(|a)*", "aa"),
    ("e", "()", ""),
    ("e", "(a)(b)(c)", "abc"),
    ("e", "((a)(b))c", "abc"),
    ("e", "(a)|(b)", "b"),
    ("e", "(a)*", "aaa"),
    ("e", "[abc]", "c"), ("e", "[abc]", "d"),
    ("e", "[a-z]+", "9abc9"), ("e", "[^a-z]+", "abc99abc"),
    ("e", "[]a]", "]"), ("e", "[^]a]", "b"),
    ("e", "[a-]", "-"), ("e", "[-a]", "-"),
    ("e", "[[:digit:]]+", "ab123cd"),
    ("e", "[[:alpha:][:digit:]]+", "-ab12-"),
    ("e", "[[:space:]]", "a b"),
    ("e", "[[:upper:]]+", "abCDef"),
    ("e", "[[:punct:]]", "a,b"),
    ("e", "[[:xdigit:]]+", "zzdeadbeefzz"),
    ("e", "[[:blank:]]+", "a \t b"),
    ("e", "\\.", "a.b"), ("e", "\\.", "axb"),
    ("e", "a\\*b", "a*b"),
    ("e", "\\(", "("), ("e", "\\[", "["), ("e", "\\\\", "\\"),
    ("e", "\\|", "|"), ("e", "\\+", "+"), ("e", "\\?", "?"),
    ("ei", "ABC", "abc"), ("ei", "[a-c]+", "ABC"), ("ei", "[^a-c]+", "ABCd"),
    ("e", "x*y*z*", ""),
    ("e", "(a+)(b+)", "aabb"),
    ("e", "(a+)+b", "aaab"),
    ("e", "[0-9]{1,3}\\.[0-9]{1,3}", "ip 192.168 here"),
    ("es", "(a)(b)", "ab"),          # nosub: no group offsets
    ("en", "^b", "a\nb"),
    ("en", "a$", "a\nb"),
    ("en", ".", "\n"),
    ("en", "[^x]", "\n"),
    ("e", ".", "\n"),
    # the basic dialect, where the operators wear backslashes
    ("-", "a\\|b", "b"),
    ("-", "\\(ab\\)*", "abab"),
    ("-", "a\\{2\\}", "aaa"),
    ("-", "a+", "a+"),
    ("-", "a?", "a?"),
    ("-", "(a)", "(a)"),
    ("-", "^a", "ba"),
    ("-", "a$", "ab"),
    ("-", "a*b", "aab"),
    ("-", "[abc]*", "cab"),
    # things that should be refused by both
    ("e", "a(", "a"),
    ("e", "a[", "a"),
    ("e", "a\\", "a"),
    ("e", "[z-a]", "a"),
    ("e", "[[:nosuch:]]", "a"),
    ("e", "a{3,1}", "a"),
]
for f, p, s in BASICS:
    case(f, p, s)

# --- a long subject, to prove the stack does not grow with it -------------
case("e", "\"([^\"]*)\"", '"' + "x" * 40000 + '"')
case("e", "a[^b]*c", "a" + "z" * 40000 + "c")
case("e", ".*", "y" * 40000)

# --------------------------------------------------------------------------

inp = "".join("%s\t%s\t%s\n" % (f, esc(p), esc(s)) for f, p, s in CASES)
open(OUT + "/cases.txt", "w").write(inp)

for name, extra in (("host", []), ("mine", ["-I", HOME + "/libc/include",
                                            HOME + "/libc/src/regex.c"])):
    src = [HOME + "/tools/regex_probe.c"]
    r = subprocess.run(["gcc", "-O1", "-g", "-Wall", "-Wextra",
                        "-o", OUT + "/" + name] + src + extra,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("could not build the %s probe:\n%s" % (name, r.stderr))
    if r.stderr.strip():
        print("warnings building %s:\n%s" % (name, r.stderr.strip()))

out = {}
for name in ("host", "mine"):
    r = subprocess.run([OUT + "/" + name], input=inp,
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        sys.exit("the %s probe exited %d\n%s" % (name, r.returncode, r.stderr))
    out[name] = r.stdout.splitlines()

if len(out["host"]) != len(out["mine"]):
    sys.exit("the probes answered a different number of cases: %d and %d"
             % (len(out["host"]), len(out["mine"])))

if len(out["host"]) != len(CASES):
    sys.exit("the probe answered %d of %d cases -- a case was cut in two"
             % (len(out["host"]), len(CASES)))

same, expected = 0, []
diffs = []
for i, (a, b) in enumerate(zip(out["host"], out["mine"])):
    f, p, s = CASES[i]
    shown = s if len(s) <= 60 else s[:57] + "..."
    if a == b:
        if i in KNOWN:
            diffs.append((f, p, shown, a, b,
                          "expected to differ and did not: " + KNOWN[i]))
        else:
            same += 1
    elif i in KNOWN:
        expected.append((f, p, shown, a, b, KNOWN[i]))
    else:
        diffs.append((f, p, shown, a, b, None))

print("%d cases: %d agree with the host's regex, %d differ as documented"
      % (len(CASES), same, len(expected)))

for f, p, s, a, b, why in expected:
    print("\n  as documented -- %s" % why)
    print("    pattern %s" % esc(p))
    print("    subject %s" % esc(s))
    print("    host: %s" % a)
    print("    ours: %s" % b)

if diffs:
    print("\n%d UNEXPECTED:\n" % len(diffs))
    for f, p, s, a, b, why in diffs:
        if why:
            print("  %s" % why)
        print("  flags %-3s  pattern %s" % (f, esc(p)))
        print("             subject %s" % esc(s))
        print("             host: %s" % a)
        print("             ours: %s\n" % b)
    sys.exit(1)
