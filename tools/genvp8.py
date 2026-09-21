#!/usr/bin/env python3
"""Generate userland/vp8_tables.h from RFC 6386.

VP8 is defined by a reference decoder printed inside its own specification,
and the constants that decoder needs run to some three thousand numbers: the
coefficient probabilities, the ones that say how those may be updated, the
intra mode probabilities indexed by both neighbours, and the two quantiser
lookups. Typing those out is not work a person should do -- one wrong digit
gives a picture that is subtly wrong in a way no amount of reading finds.

So they are lifted from the document itself, the same way tools/genroots.py
lifts the certificate authorities from Mozilla's list. Run it when the tables
need regenerating; the output is checked in so an ordinary build needs no
network.

    python3 tools/genvp8.py            # fetches the RFC and writes the header
    python3 tools/genvp8.py rfc.txt    # uses a local copy instead
"""

import os
import re
import sys
import urllib.request

RFC_URL = "https://www.rfc-editor.org/rfc/rfc6386.txt"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "userland", "vp8_tables.h")


def fetch(path=None):
    if path:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    with urllib.request.urlopen(RFC_URL, timeout=60) as r:
        return r.read().decode("utf-8", "replace")


def strip_pagination(text):
    """Remove the running headers, footers and page breaks.

    They fall in the middle of the longer tables, so leaving them in would put
    a page number where a probability belongs.
    """
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("Bankoski"):
            continue
        if s.startswith("RFC 6386") and "November 2011" in s:
            continue
        if s == "\f" or line.startswith("\f"):
            continue
        out.append(line)
    return "\n".join(out)


def parse_enums(text):
    """Every enumerator in the document, with its value.

    The decision trees are written with the mode names as their leaves --
    "-B_PRED, 2" rather than "-4, 2" -- so the names have to be resolved
    before the trees can become numbers.
    """
    vals = {}
    for m in re.finditer(r"typedef\s+enum\s*\{(.*?)\}", text, re.S):
        body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
        nxt = 0
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                nm, rhs = item.split("=", 1)
                nm, rhs = nm.strip(), rhs.strip()
                if not re.fullmatch(r"[A-Za-z_]\w*", nm):
                    continue
                if re.fullmatch(r"-?\d+", rhs):
                    nxt = int(rhs)
                elif rhs in vals:
                    nxt = vals[rhs]
                else:
                    continue
            else:
                nm = item
                if not re.fullmatch(r"[A-Za-z_]\w*", nm):
                    continue
            vals[nm] = nxt
            nxt += 1
    return vals


def grab_tree(text, name, want, enums):
    """A decision tree, with its symbolic leaves resolved to numbers."""
    starts = [m.start() for m in
              re.finditer(r"\b" + re.escape(name) + r"\s*\[", text)]
    for start in reversed(starts):
        brace = text.find("{", start)
        if brace < 0:
            continue
        depth, i = 0, brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = re.sub(r"/\*.*?\*/", " ", text[brace + 1:i], flags=re.S)
        out = []
        ok = True
        for tok in body.split(","):
            tok = tok.strip()
            if not tok:
                continue
            neg = tok.startswith("-")
            if neg:
                tok = tok[1:].strip()
            if re.fullmatch(r"\d+", tok):
                v = int(tok)
            elif tok in enums:
                v = enums[tok]
            else:
                ok = False
                break
            out.append(-v if neg else v)
        if ok and len(out) == want:
            return out
    raise SystemExit("genvp8: could not read the tree %s" % name)


def grab(text, name, want=None):
    """The integers of the initialiser for `name`.

    Finds the LAST definition, because the specification states several tables
    twice -- once in the prose and once again in the reference decoder -- and
    the second is the one the decoder actually compiles.
    """
    starts = [m.start() for m in
              re.finditer(r"\b" + re.escape(name) + r"\s*(\[|=)", text)]
    if not starts:
        raise SystemExit("genvp8: %s is not in the document" % name)

    for start in reversed(starts):
        eq = text.find("=", start)
        brace = text.find("{", start)
        if eq < 0 or brace < 0 or brace < eq:
            continue
        # Walk to the matching close brace.
        depth, i = 0, brace
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[brace:i + 1]
        body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
        nums = [int(t) for t in re.findall(r"-?\d+", body)]
        if want is None or len(nums) == want:
            return nums
        # Wrong length: try an earlier definition.
    raise SystemExit("genvp8: %s came out the wrong length (wanted %s)"
                     % (name, want))


def emit(f, ctype, name, dims, values, per_line=12, comment=None):
    total = 1
    for d in dims:
        total *= d
    if len(values) != total:
        raise SystemExit("genvp8: %s has %d values, expected %d"
                         % (name, len(values), total))
    if comment:
        f.write("/* %s */\n" % comment)
    f.write("static const %s %s%s = {\n" % (ctype, name,
                                            "".join("[%d]" % d for d in dims)))
    # Break the innermost dimension onto its own lines so the shape shows.
    inner = dims[-1]
    for i in range(0, total, inner):
        chunk = values[i:i + inner]
        if inner <= per_line:
            f.write("    " + ", ".join("%d" % v for v in chunk) + ",\n")
        else:
            for j in range(0, inner, per_line):
                f.write("    " + ", ".join("%d" % v for v in chunk[j:j + per_line]) + ",\n")
    f.write("};\n\n")


def main():
    text = strip_pagination(fetch(sys.argv[1] if len(sys.argv) > 1 else None))
    enums = parse_enums(text)
    for need in ("B_PRED", "DC_PRED", "B_HU_PRED", "dct_eob", "dct_cat6"):
        if need not in enums:
            raise SystemExit("genvp8: the enumerations did not parse (%s missing)"
                             % need)

    tables = []

    # --- trees. Negative entries are leaves, so these stay signed. ---------
    tables.append(("signed char", "vp8_kf_ymode_tree", [8],
                   grab_tree(text, "kf_ymode_tree", 8, enums), "keyframe luma mode"))
    tables.append(("signed char", "vp8_uv_mode_tree", [6],
                   grab_tree(text, "uv_mode_tree", 6, enums), "chroma mode"))
    tables.append(("signed char", "vp8_bmode_tree", [18],
                   grab_tree(text, "bmode_tree", 18, enums), "the ten 4x4 luma modes"))
    tables.append(("signed char", "vp8_segment_tree", [6],
                   grab_tree(text, "mb_segment_tree", 6, enums),
                   "which segment a block is in"))
    tables.append(("signed char", "vp8_coeff_tree", [22],
                   grab_tree(text, "coeff_tree", 22, enums),
                   "one DCT coefficient token"))

    # --- mode probabilities ------------------------------------------------
    tables.append(("unsigned char", "vp8_kf_ymode_prob", [4],
                   grab(text, "kf_ymode_prob", 4), None))
    tables.append(("unsigned char", "vp8_kf_uv_mode_prob", [3],
                   grab(text, "kf_uv_mode_prob", 3), None))
    tables.append(("unsigned char", "vp8_kf_bmode_prob", [10, 10, 9],
                   grab(text, "kf_bmode_prob", 900),
                   "indexed by the modes above and to the left"))

    # --- coefficients ------------------------------------------------------
    tables.append(("int", "vp8_coeff_bands", [16],
                   grab(text, "coeff_bands", 16),
                   "which probability band each position falls in"))
    tables.append(("unsigned char", "vp8_coeff_update_probs", [4, 8, 3, 11],
                   grab(text, "coeff_update_probs", 1056),
                   "how likely each probability below is to be replaced"))
    tables.append(("unsigned char", "vp8_default_coeff_probs", [4, 8, 3, 11],
                   grab(text, "default_coeff_probs", 1056), None))

    # --- the extra-bit probabilities for the six token categories ----------
    for i in range(1, 7):
        v = grab(text, "Pcat%d" % i)
        tables.append(("unsigned char", "vp8_pcat%d" % i, [len(v)], v, None))

    # --- quantiser lookups --------------------------------------------------
    tables.append(("short", "vp8_dc_qlookup", [128],
                   grab(text, "dc_qlookup", 128), None))
    tables.append(("short", "vp8_ac_qlookup", [128],
                   grab(text, "ac_qlookup", 128), None))

    with open(OUT, "w") as f:
        f.write("""#ifndef VP8_TABLES_H
#define VP8_TABLES_H

/* GENERATED by tools/genvp8.py from RFC 6386 -- do not edit by hand.
 *
 * These are the constants the VP8 bitstream is defined in terms of: the
 * decision trees for every syntax element, the probabilities the arithmetic
 * coder is primed with, and the two quantiser lookups. About three thousand
 * numbers, lifted from the reference decoder printed inside the specification
 * rather than retyped, because a single wrong digit here does not fail -- it
 * quietly returns the wrong picture.
 *
 * Trees are signed: a negative entry is a leaf holding -value, a positive one
 * is the index of the next pair.
 */

""")
        for ctype, name, dims, values, comment in tables:
            emit(f, ctype, name, dims, values, comment=comment)
        f.write("#endif /* VP8_TABLES_H */\n")

    print("wrote %s" % os.path.normpath(OUT))
    print("  modes: B_PRED=%d num_ymodes=%d num_intra_bmodes=%d dct_eob=%d"
          % (enums["B_PRED"], enums["num_ymodes"], enums["num_intra_bmodes"],
             enums["dct_eob"]))
    for _, name, dims, values, _ in tables:
        print("  %-28s %s (%d values)" %
              (name, "".join("[%d]" % d for d in dims), len(values)))


if __name__ == "__main__":
    main()
