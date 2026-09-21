#!/usr/bin/env python3
"""The browser no longer guesses whether the body is in pieces.

The kernel now rejoins a chunked body on both of its paths and renames the
header afterwards, so there is one answer where there used to be two. The
walk that tried to tell them apart by reading the bytes goes away with them.
"""
import re, sys
import os

# Where things are. The project is found from this file's own location, so a
# checkout anywhere works; the scratch area where NetSurf and Lexbor sources
# are unpacked defaults to ~/src. Both can be overridden by environment.
XYUOS = os.environ.get(
    "XYUOS", os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XYUOS_SRC = os.environ.get(
    "XYUOS_SRC", os.path.join(os.path.expanduser("~"), "src"))

PATH = XYUOS + "/third_party/nsxyuos/fetch_xyuos.c"

with open(PATH) as f:
    s = f.read()

# --- 1. the dechunker itself, and its one call -----------------------------
start = s.index("/* Undo Transfer-Encoding: chunked, in place.")
end = s.index("static void finish(struct fetch_xyuos_context *c, int n) {")
s = s[:start] + s[end:]

call = """    /* The pieces first: what follows is gzip only once they are joined.
     *
     * A refusal is not an error. It means this body is not chunked -- which
     * over https is the normal case, because the kernel has already joined
     * it and only the header still says otherwise. */
    if (chunked && len > 0) {
        int joined = dechunk((unsigned char *)rxbuf + body, len);
        if (joined > 0) len = joined;
    }

"""
if call not in s:
    sys.exit("the call to dechunk is not where it was")
s = s.replace(call, "")

# --- 2. headers: two passes, so order stops mattering ----------------------
old_head = s[s.index("static void deliver_headers("):
             s.index("static void finish(struct fetch_xyuos_context *c, int n) {")]

new_head = '''static int header_is(const char *line, const char *name) {
    return strncasecmp(line, name, strlen(name)) == 0;
}

/* Hands each header line to NetSurf, and picks out of them the two things
 * this code needs to act on itself.
 *
 * Read twice on purpose. Whether Content-Length still describes anything
 * depends on headers that may come after it -- a server is free to send it
 * before Content-Encoding -- so the flags are all gathered before a single
 * line is passed on. */
static void deliver_headers(struct fetch_xyuos_context *c,
                            char *head, int headlen,
                            char *location, int locmax, int *gzipped,
                            int *chunked) {
    location[0] = 0;
    *gzipped = 0;
    *chunked = 0;

    /* Past the status line: NetSurf is told the code separately. */
    int first = 0;
    while (first < headlen && head[first] != '\\n') first++;
    first++;

    for (int pass = 0; pass < 2; pass++) {
        int at = first;
        while (at < headlen) {
            int end = at;
            while (end < headlen && head[end] != '\\n') end++;
            int stop = end;
            while (stop > at && (head[stop - 1] == '\\r' || head[stop - 1] == ' '))
                stop--;
            if (stop <= at) { at = end + 1; continue; }

            char save = head[stop];
            head[stop] = 0;
            char *line = head + at;

            /* Anything that would describe the body as it arrived rather
             * than as NetSurf is about to receive it. */
            int drop = header_is(line, "content-encoding:")
                    || header_is(line, "transfer-encoding:")
                    || header_is(line, "x-xyuos-dechunked:")
                    || ((*gzipped || *chunked) &&
                        header_is(line, "content-length:"));

            if (pass == 0) {
                if (header_is(line, "location:")) {
                    const char *v = line + 9;
                    while (*v == ' ') v++;
                    snprintf(location, (size_t)locmax, "%s", v);
                }
                /* The body is handed on in the clear, so this would be a lie
                 * by the time NetSurf saw it. */
                if (header_is(line, "content-encoding:")) {
                    const char *v = line + 17;
                    while (*v == ' ') v++;
                    if (*v == 'g' || *v == 'x' || *v == 'd') *gzipped = 1;
                }
                /* The kernel put the pieces back together and renamed the
                 * header to say so. Either name means the length that came
                 * with it counted framing, not content. */
                if (header_is(line, "transfer-encoding:") ||
                    header_is(line, "x-xyuos-dechunked:")) {
                    const char *v = line + 18;
                    while (*v == ' ') v++;
                    if (strncasecmp(v, "chunked", 7) == 0) *chunked = 1;
                }
            } else if (!drop && !c->aborted) {
                fetch_msg msg;
                msg.type = FETCH_HEADER;
                msg.data.header_or_data.buf = (const uint8_t *)line;
                msg.data.header_or_data.len = (size_t)(stop - at);
                send_msg(&msg, c);
            }

            head[stop] = save;
            at = end + 1;
        }
    }
}

'''
s = s.replace(old_head, new_head)

if "dechunk(" in s:
    sys.exit("something still calls dechunk")

with open(PATH, "w") as f:
    f.write(s)
print("patched " + PATH)
