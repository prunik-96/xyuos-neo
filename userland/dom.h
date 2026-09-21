#ifndef DOM_H
#define DOM_H

/* The document as a tree.
 *
 * The browser used to turn markup straight into a list of things to draw, in a
 * single pass. That is the right shape for a page that never changes, and the
 * wrong one the moment a script can say
 *
 *     document.getElementById("count").textContent = "3"
 *
 * because there has to be a "count" to find, and something to change about it.
 *
 * So parsing builds a tree now, and the drawing list is produced by walking it.
 * The drawing itself did not change -- the same code runs, driven by the tree
 * instead of by the tokenizer -- but it can be run AGAIN after a script has
 * altered something, which is the whole point.
 *
 * Nodes are indices rather than pointers, because a script holds on to them
 * and the arrays may move. Text and attributes are not copied: they point into
 * the source buffer, which lives as long as the tree does. Anything a script
 * writes goes into a separate buffer that grows and is never compacted -- a
 * page that rewrites the same text a hundred thousand times will run it dry,
 * which is a better failure than a dangling pointer.
 */

#include <stdlib.h>
#include <string.h>

#define DOM_MAX_NODES 30000
#define DOM_EDIT_CAP  (512 * 1024)
#define DOM_TAG_MAX   24
#define DOM_DEPTH_MAX 96

enum { DOM_ELEM, DOM_TEXT };

typedef struct {
    unsigned char kind;
    char tag[DOM_TAG_MAX];          /* lowercased, elements only */
    const char *attr;  int attrlen; /* the raw text between name and '>' */
    const char *text;  int textlen; /* text nodes, and raw-text elements */
    int parent, first, last, next;
} dom_node;

static dom_node *dom;
static int       dom_n;
static char     *dom_edit;
static int       dom_editlen;
static int       dom_root = -1;

static int dom_init(void) {
    dom = (dom_node *)malloc((size_t)DOM_MAX_NODES * sizeof(dom_node));
    dom_edit = (char *)malloc(DOM_EDIT_CAP);
    return dom && dom_edit;
}

/* Somewhere to put a string a script made up. */
static const char *dom_intern(const char *s, int len, int *outlen) {
    if (len < 0) len = 0;
    if (dom_editlen + len + 1 > DOM_EDIT_CAP) { *outlen = 0; return ""; }
    char *p = dom_edit + dom_editlen;
    for (int i = 0; i < len; i++) p[i] = s[i];
    p[len] = 0;
    dom_editlen += len + 1;
    *outlen = len;
    return p;
}

static int dom_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int dom_ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Elements that never have children. */
static int dom_void(const char *t) {
    static const char *V[] = { "br","hr","img","input","meta","link","source",
        "area","base","col","embed","param","track","wbr", 0 };
    for (int i = 0; V[i]; i++) if (dom_ci_eq(t, V[i])) return 1;
    return 0;
}

/* Elements whose contents are not markup. Everything up to the closing tag is
 * one text node -- which is how a script's source survives the parser. */
static int dom_rawtext(const char *t) {
    return dom_ci_eq(t, "script") || dom_ci_eq(t, "style") ||
           dom_ci_eq(t, "textarea") || dom_ci_eq(t, "title") ||
           /* Not HTML, and not to be parsed as any: an <svg> is a picture
            * written out in full, and the browser has a rasteriser for
            * exactly that. Kept whole, as its own source, for it to read. */
           dom_ci_eq(t, "svg");
}

/* Subtrees this browser has no use for and will not draw. Kept out of the tree
 * entirely rather than walked and discarded. */
static int dom_skip(const char *t) {
    return dom_ci_eq(t, "template");
}

/* An open element that a new one implicitly ends. Real pages leave these open
 * constantly, and without the rule the tree nests until it hits the depth
 * limit and the rest of the page is lost. */
static int dom_closes(const char *open, const char *now) {
    if (dom_ci_eq(open, "p"))
        return dom_ci_eq(now, "p") || dom_ci_eq(now, "div") ||
               dom_ci_eq(now, "ul") || dom_ci_eq(now, "ol") ||
               dom_ci_eq(now, "table") || dom_ci_eq(now, "blockquote") ||
               dom_ci_eq(now, "pre") || dom_ci_eq(now, "section") ||
               dom_ci_eq(now, "h1") || dom_ci_eq(now, "h2") ||
               dom_ci_eq(now, "h3") || dom_ci_eq(now, "h4");
    if (dom_ci_eq(open, "li")) return dom_ci_eq(now, "li");
    if (dom_ci_eq(open, "dt") || dom_ci_eq(open, "dd"))
        return dom_ci_eq(now, "dt") || dom_ci_eq(now, "dd");
    if (dom_ci_eq(open, "td") || dom_ci_eq(open, "th"))
        return dom_ci_eq(now, "td") || dom_ci_eq(now, "th") || dom_ci_eq(now, "tr");
    if (dom_ci_eq(open, "tr")) return dom_ci_eq(now, "tr");
    if (dom_ci_eq(open, "option")) return dom_ci_eq(now, "option");
    return 0;
}

static int dom_add(int parent, int kind) {
    if (dom_n >= DOM_MAX_NODES) return -1;
    int i = dom_n++;
    dom_node *d = &dom[i];
    memset(d, 0, sizeof *d);
    d->kind = (unsigned char)kind;
    d->parent = parent;
    d->first = d->last = d->next = -1;
    if (parent >= 0) {
        if (dom[parent].last >= 0) dom[dom[parent].last].next = i;
        else dom[parent].first = i;
        dom[parent].last = i;
    }
    return i;
}

/* Detach every child of `n`. The nodes themselves are left where they are;
 * nothing here is ever freed individually. */
static void dom_clear_children(int n) {
    if (n < 0) return;
    dom[n].first = dom[n].last = -1;
}

static int dom_new_text(int parent, const char *s, int len) {
    int i = dom_add(parent, DOM_TEXT);
    if (i < 0) return -1;
    dom[i].text = s;
    dom[i].textlen = len;
    return i;
}

/* --- parsing --------------------------------------------------------------
 * The same tokenizer the browser always had: text until '<', then a tag. What
 * is new is that the result is a tree rather than a stream of drawing
 * instructions. */

static void dom_parse(const char *h, int len) {
    dom_n = 0;
    dom_editlen = 0;
    dom_root = dom_add(-1, DOM_ELEM);
    if (dom_root < 0) return;
    snprintf(dom[dom_root].tag, DOM_TAG_MAX, "%s", "#document");

    int stack[DOM_DEPTH_MAX];
    int depth = 0;
    stack[depth++] = dom_root;

    int text_start = -1;

    for (int i = 0; i < len; ) {
        if (h[i] != '<') {
            if (text_start < 0) text_start = i;
            i++;
            continue;
        }

        /* Close off whatever text ran up to this tag. */
        if (text_start >= 0) {
            dom_new_text(stack[depth - 1], h + text_start, i - text_start);
            text_start = -1;
        }

        if (i + 3 < len && h[i+1] == '!' && h[i+2] == '-' && h[i+3] == '-') {
            i += 4;
            while (i + 2 < len && !(h[i] == '-' && h[i+1] == '-' && h[i+2] == '>')) i++;
            i = (i + 3 < len) ? i + 3 : len;
            continue;
        }
        if (i + 1 < len && (h[i+1] == '!' || h[i+1] == '?')) {
            while (i < len && h[i] != '>') i++;
            i = (i < len) ? i + 1 : len;
            continue;
        }

        int j = i + 1;
        int closing = 0;
        if (j < len && h[j] == '/') { closing = 1; j++; }

        char name[DOM_TAG_MAX];
        int n = 0;
        while (j < len && !dom_isspace(h[j]) && h[j] != '>' && h[j] != '/' &&
               n < DOM_TAG_MAX - 1) {
            int c = h[j++];
            if (c >= 'A' && c <= 'Z') c += 32;
            name[n++] = (char)c;
        }
        name[n] = 0;

        int attr_start = j;
        {
            /* A `>` inside a quoted attribute value belongs to the value, not
             * to the end of the tag. Pages put inline CSS and whole JSON
             * documents in attributes and both are full of them: scanning
             * straight to the first `>` ends the tag in the middle of itself
             * and spills the rest onto the page as text. Wikipedia does this
             * on every article. */
            char q = 0;
            while (j < len) {
                char c = h[j];
                if (q) { if (c == q) q = 0; }
                else if (c == '"' || c == '\'') q = c;
                else if (c == '>') break;
                j++;
            }
        }
        int attr_len = j - attr_start;
        int next = (j < len) ? j + 1 : len;

        if (!name[0]) { i = next; continue; }

        if (closing) {
            for (int d = depth - 1; d > 0; d--) {
                if (dom_ci_eq(dom[stack[d]].tag, name)) { depth = d; break; }
            }
            i = next;
            continue;
        }

        /* A subtree we will not draw: step over it without recording it. */
        if (dom_skip(name)) {
            char end[DOM_TAG_MAX + 4];
            snprintf(end, sizeof end, "</%s", name);
            int el = (int)strlen(end);
            int k = next;
            while (k + el <= len) {
                int m = 0;
                while (m < el) {
                    int x = h[k+m], y = end[m];
                    if (x >= 'A' && x <= 'Z') x += 32;
                    if (x != y) break;
                    m++;
                }
                if (m == el) break;
                k++;
            }
            while (k < len && h[k] != '>') k++;
            i = (k < len) ? k + 1 : len;
            continue;
        }

        /* An unclosed paragraph, list item or table cell ends here. */
        while (depth > 1 && dom_closes(dom[stack[depth - 1]].tag, name)) depth--;

        int self_closing = (attr_len > 0 && h[attr_start + attr_len - 1] == '/');
        int e = dom_add(stack[depth - 1], DOM_ELEM);
        if (e < 0) return;
        snprintf(dom[e].tag, DOM_TAG_MAX, "%s", name);
        dom[e].attr = h + attr_start;
        dom[e].attrlen = attr_len;

        if (dom_rawtext(name)) {
            char end[DOM_TAG_MAX + 4];
            snprintf(end, sizeof end, "</%s", name);
            int el = (int)strlen(end);
            int k = next;
            while (k + el <= len) {
                int m = 0;
                while (m < el) {
                    int x = h[k+m], y = end[m];
                    if (x >= 'A' && x <= 'Z') x += 32;
                    if (x != y) break;
                    m++;
                }
                if (m == el) break;
                k++;
            }
            if (k > next) dom_new_text(e, h + next, k - next);
            while (k < len && h[k] != '>') k++;
            i = (k < len) ? k + 1 : len;
            continue;
        }

        if (!dom_void(name) && !self_closing && depth < DOM_DEPTH_MAX)
            stack[depth++] = e;

        i = next;
    }

    if (text_start >= 0 && text_start < len)
        dom_new_text(stack[depth - 1], h + text_start, len - text_start);
}

/* --- looking things up ----------------------------------------------------- */

/* The value of one attribute, as a pointer into the source. Returns 0 when the
 * attribute is absent. Quoted values are returned without their quotes; the
 * caller decodes entities if it cares. */
static const char *dom_attr_raw(int node, const char *want, int *outlen) {
    if (node < 0 || dom[node].kind != DOM_ELEM) return 0;
    const char *a = dom[node].attr;
    int alen = dom[node].attrlen;
    if (!a) return 0;
    int wl = (int)strlen(want);
    for (int i = 0; i + wl <= alen; i++) {
        if (i && !dom_isspace(a[i-1])) continue;
        int k = 0;
        while (k < wl) {
            int x = a[i+k], y = want[k];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != y) break;
            k++;
        }
        if (k != wl) continue;
        int j = i + wl;
        while (j < alen && dom_isspace(a[j])) j++;
        if (j >= alen || a[j] != '=') continue;
        j++;
        while (j < alen && dom_isspace(a[j])) j++;
        int quote = 0;
        if (j < alen && (a[j] == '"' || a[j] == '\'')) quote = a[j++];
        int start = j;
        while (j < alen) {
            if (quote ? a[j] == quote : (dom_isspace(a[j]) || a[j] == '>')) break;
            j++;
        }
        *outlen = j - start;
        return a + start;
    }
    return 0;
}

static int dom_attr_eq(int node, const char *want, const char *value) {
    int n = 0;
    const char *v = dom_attr_raw(node, want, &n);
    if (!v) return 0;
    int vl = (int)strlen(value);
    if (n != vl) return 0;
    for (int i = 0; i < n; i++) if (v[i] != value[i]) return 0;
    return 1;
}

static int dom_find_id_from(int node, const char *id) {
    for (int i = node; i >= 0 && i < dom_n; i++)
        if (dom[i].kind == DOM_ELEM && dom_attr_eq(i, "id", id)) return i;
    return -1;
}

static int dom_find_id(const char *id) { return dom_find_id_from(0, id); }

/* Does this element carry `cls` among its classes? */
static int dom_has_class(int node, const char *cls) {
    int n = 0;
    const char *v = dom_attr_raw(node, "class", &n);
    if (!v) return 0;
    int cl = (int)strlen(cls);
    for (int i = 0; i <= n - cl; i++) {
        if (i && !dom_isspace(v[i-1])) continue;
        if (i + cl < n && !dom_isspace(v[i+cl])) continue;
        int k = 0;
        while (k < cl && v[i+k] == cls[k]) k++;
        if (k == cl) return 1;
    }
    return 0;
}

/* Append every character of the subtree's text to `out`. What textContent
 * reads. */
static int dom_text_into(int node, char *out, int max, int n) {
    if (node < 0) return n;
    if (dom[node].kind == DOM_TEXT) {
        for (int i = 0; i < dom[node].textlen && n < max - 1; i++)
            out[n++] = dom[node].text[i];
        return n;
    }
    for (int c = dom[node].first; c >= 0; c = dom[c].next) {
        /* An <svg> holds its own source, kept as text because that is the
         * only way to keep it whole -- but it is a drawing, not words. Read
         * as text it puts `<path d="m6.427 4.427 ...` into the label of every
         * button on the page that has an icon in it. */
        if (dom[c].kind == DOM_ELEM && dom_ci_eq(dom[c].tag, "svg")) continue;
        n = dom_text_into(c, out, max, n);
    }
    return n;
}

/* --- what a script does to the tree ------------------------------------------
 *
 * Attributes are stored as the raw text between the tag name and the '>', so
 * changing one means writing that text out again with the new value in it.
 * That is more work than a table of attributes would be, and it keeps every
 * reader in the file -- the CSS matcher, the renderer, the script -- looking
 * at exactly the same thing. */

static void dom_set_attr(int node, const char *name, const char *value) {
    if (node < 0 || dom[node].kind != DOM_ELEM) return;
    static char buf[2048];
    int out = 0;

    const char *a = dom[node].attr;
    int alen = dom[node].attrlen;
    int nl = (int)strlen(name);

    /* Copy every attribute except the one being replaced. */
    int i = 0;
    while (a && i < alen) {
        while (i < alen && (dom_isspace(a[i]) || a[i] == '/')) i++;
        if (i >= alen) break;
        int ns = i;
        while (i < alen && !dom_isspace(a[i]) && a[i] != '=' && a[i] != '>') i++;
        int thisn = i - ns;
        int vs = -1, ve = -1;
        while (i < alen && dom_isspace(a[i])) i++;
        if (i < alen && a[i] == '=') {
            i++;
            while (i < alen && dom_isspace(a[i])) i++;
            int q = 0;
            if (i < alen && (a[i] == '"' || a[i] == '\'')) q = a[i++];
            vs = i;
            while (i < alen && (q ? a[i] != q : !dom_isspace(a[i]))) i++;
            ve = i;
            if (q && i < alen) i++;
        }
        int same = (thisn == nl);
        if (same)
            for (int k = 0; k < nl; k++) {
                int x = a[ns + k], y = name[k];
                if (x >= 'A' && x <= 'Z') x += 32;
                if (y >= 'A' && y <= 'Z') y += 32;
                if (x != y) { same = 0; break; }
            }
        if (same) continue;
        if (out && out < (int)sizeof buf - 2) buf[out++] = ' ';
        for (int k = 0; k < thisn && out < (int)sizeof buf - 2; k++) buf[out++] = a[ns + k];
        if (vs >= 0) {
            if (out < (int)sizeof buf - 3) { buf[out++] = '='; buf[out++] = '"'; }
            for (int k = vs; k < ve && out < (int)sizeof buf - 2; k++) buf[out++] = a[k];
            if (out < (int)sizeof buf - 2) buf[out++] = '"';
        }
    }

    if (value) {
        if (out && out < (int)sizeof buf - 2) buf[out++] = ' ';
        out += snprintf(buf + out, sizeof buf - (size_t)out, "%s=\"%s\"", name, value);
        if (out > (int)sizeof buf - 1) out = (int)sizeof buf - 1;
    }
    buf[out] = 0;

    int kept = 0;
    const char *p = dom_intern(buf, out, &kept);
    dom[node].attr = p;
    dom[node].attrlen = kept;
}

/* An element with no parent yet. */
static int dom_create(const char *tag) {
    int i = dom_add(-1, DOM_ELEM);
    if (i < 0) return -1;
    snprintf(dom[i].tag, DOM_TAG_MAX, "%s", tag);
    dom[i].attr = "";
    dom[i].attrlen = 0;
    return i;
}

static void dom_detach(int node) {
    if (node <= 0) return;
    int p = dom[node].parent;
    if (p < 0) return;
    int prev = -1;
    for (int c = dom[p].first; c >= 0; c = dom[c].next) {
        if (c == node) break;
        prev = c;
    }
    if (prev >= 0) dom[prev].next = dom[node].next;
    else dom[p].first = dom[node].next;
    if (dom[p].last == node) dom[p].last = prev;
    dom[node].parent = -1;
    dom[node].next = -1;
}

static void dom_append(int parent, int child) {
    if (parent < 0 || child < 0 || parent == child) return;
    dom_detach(child);
    dom[child].parent = parent;
    dom[child].next = -1;
    if (dom[parent].last >= 0) dom[dom[parent].last].next = child;
    else dom[parent].first = child;
    dom[parent].last = child;
}

/* Write a subtree back out as markup. `self` includes the element's own tag.
 * Enough for innerHTML to read back roughly what it was given. */
static int dom_html_into(int node, char *out, int max, int n, int self) {
    if (node < 0) return n;
    if (dom[node].kind == DOM_TEXT) {
        for (int i = 0; i < dom[node].textlen && n < max - 1; i++)
            out[n++] = dom[node].text[i];
        return n;
    }
    if (self) {
        n += snprintf(out + n, (size_t)(max - n), "<%s", dom[node].tag);
        if (dom[node].attrlen && n < max - 2) {
            out[n++] = ' ';
            for (int i = 0; i < dom[node].attrlen && n < max - 1; i++)
                out[n++] = dom[node].attr[i];
        }
        if (n < max - 1) out[n++] = '>';
    }
    for (int c = dom[node].first; c >= 0; c = dom[c].next)
        n = dom_html_into(c, out, max, n, 1);
    if (self && !dom_void(dom[node].tag))
        n += snprintf(out + n, (size_t)(max - n), "</%s>", dom[node].tag);
    if (n > max - 1) n = max - 1;
    return n;
}

/* Parse a fragment and make it the contents of `node`.
 *
 * The parser builds into the same arrays, so this appends nodes at the end and
 * then re-parents the ones that came out at the top of the fragment. The text
 * has to be kept, because the nodes point into it rather than copying it. */
static void dom_set_html(int node, const char *html, int len) {
    if (node < 0 || dom[node].kind != DOM_ELEM) return;
    int kept = 0;
    const char *src = dom_intern(html, len, &kept);

    dom_clear_children(node);

    /* A miniature of the main parser: only elements and text, no raw-text
     * elements and no skipped subtrees, which a fragment does not need. */
    int stack[DOM_DEPTH_MAX];
    int depth = 0;
    stack[depth++] = node;
    int text_start = -1;

    for (int i = 0; i < kept; ) {
        if (src[i] != '<') { if (text_start < 0) text_start = i; i++; continue; }
        if (text_start >= 0) {
            dom_new_text(stack[depth-1], src + text_start, i - text_start);
            text_start = -1;
        }
        if (i + 3 < kept && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
            i += 4;
            while (i + 2 < kept && !(src[i]=='-'&&src[i+1]=='-'&&src[i+2]=='>')) i++;
            i = (i + 3 < kept) ? i + 3 : kept;
            continue;
        }
        int j = i + 1, closing = 0;
        if (j < kept && src[j] == '/') { closing = 1; j++; }
        char name[DOM_TAG_MAX];
        int k = 0;
        while (j < kept && !dom_isspace(src[j]) && src[j] != '>' && src[j] != '/' &&
               k < DOM_TAG_MAX - 1) {
            int c = src[j++];
            if (c >= 'A' && c <= 'Z') c += 32;
            name[k++] = (char)c;
        }
        name[k] = 0;
        int as = j;
        while (j < kept && src[j] != '>') j++;
        int al = j - as;
        int next = (j < kept) ? j + 1 : kept;
        if (!name[0]) { i = next; continue; }

        if (closing) {
            for (int d = depth - 1; d > 0; d--)
                if (dom_ci_eq(dom[stack[d]].tag, name)) { depth = d; break; }
            i = next;
            continue;
        }
        int selfclose = (al > 0 && src[as + al - 1] == '/');
        int e = dom_add(stack[depth-1], DOM_ELEM);
        if (e < 0) return;
        snprintf(dom[e].tag, DOM_TAG_MAX, "%s", name);
        dom[e].attr = src + as;
        dom[e].attrlen = al;
        if (!dom_void(name) && !selfclose && depth < DOM_DEPTH_MAX)
            stack[depth++] = e;
        i = next;
    }
    if (text_start >= 0 && text_start < kept)
        dom_new_text(stack[depth-1], src + text_start, kept - text_start);
}

/* Replace everything inside `node` with one piece of text. */
static void dom_set_text(int node, const char *s, int len) {
    if (node < 0 || dom[node].kind != DOM_ELEM) return;
    dom_clear_children(node);
    int n = 0;
    const char *p = dom_intern(s, len, &n);
    dom_new_text(node, p, n);
}

#endif /* DOM_H */
