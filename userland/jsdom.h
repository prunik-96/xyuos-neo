#ifndef JSDOM_H
#define JSDOM_H

/* What a script can see of the page.
 *
 * The engine in js.h knows nothing about documents. This is the part that
 * hands it a `document`, wraps tree nodes as objects a script can hold, and
 * turns a click into a call.
 *
 * An element is a host object: it stores nothing of its own but an index into
 * the tree, and answers for every property by reading or writing the tree
 * itself. textContent is not a copy that has to be kept in step -- it IS the
 * text nodes underneath, computed when asked and replaced when set. That is
 * more work per access and it is the only way the two can never disagree.
 *
 * Wrappers are remembered per node, so that `a === b` is true when a and b are
 * the same element, which scripts check constantly.
 *
 * WHAT IS HERE: getElementById, querySelector and querySelectorAll for the
 * selectors css.h can already match, textContent, innerHTML, className,
 * classList, style, attributes, the tree relationships, createElement and
 * appendChild, addEventListener with bubbling, and setTimeout.
 *
 * WHAT IS NOT: layout measurements (getBoundingClientRect and friends), since
 * a script that reads them is trying to position something and we do not
 * position anything; XMLHttpRequest and fetch, which would need the network
 * inside the interpreter; and anything to do with frames or history.
 */

#include "dom.h"

/* Set whenever a script changes the page, so the browser knows to lay it out
 * again. */
static int dom_dirty;

/* The browser supplies these. */
static void (*jsd_navigate_hook)(const char *url);
static unsigned (*jsd_now_hook)(void);

/* ==========================================================================
 * wrappers
 * ========================================================================== */

#define JSD_MAX_WRAP 4096
static js_obj *jsd_wrap_obj[JSD_MAX_WRAP];
static int     jsd_wrap_node[JSD_MAX_WRAP];
static int     jsd_nwrap;

static int jsd_elem_get(js_obj *o, const char *name, js_val *out);
static int jsd_elem_set(js_obj *o, const char *name, js_val v);
static js_val jsd_nothing(js_val self, js_val *a, int n);

static js_val jsd_element(int node) {
    if (node < 0) return js_null();
    for (int i = 0; i < jsd_nwrap; i++)
        if (jsd_wrap_node[i] == node) return js_obj_val(jsd_wrap_obj[i]);

    js_obj *o = js_new(JSO_PLAIN);
    if (!o) return js_null();
    o->host = node;
    o->hget = jsd_elem_get;
    o->hset = jsd_elem_set;
    o->cname = "Element";
    if (jsd_nwrap < JSD_MAX_WRAP) {
        jsd_wrap_obj[jsd_nwrap] = o;
        jsd_wrap_node[jsd_nwrap] = node;
        jsd_nwrap++;
    }
    return js_obj_val(o);
}

/* Which tree node an object stands for, or -1 if it stands for none. Every
 * object starts at -1, so this is exact rather than a guess. */
static int jsd_node_of(js_val v) {
    if (v.t != JS_OBJ || !v.u.o) return -1;
    return v.u.o->host;
}

/* ==========================================================================
 * selectors
 * ========================================================================== */

/* The subset css.h matches, applied to one element: a tag, an #id, a .class,
 * or several of those run together. Descendant selectors ("nav a") match on
 * the last part alone, which is what the great majority of them mean anyway.
 */
static int jsd_matches(int node, const char *sel) {
    if (node < 0 || dom[node].kind != DOM_ELEM) return 0;

    /* Only the last part of a descendant selector is tested. */
    const char *last = sel;
    for (const char *p = sel; *p; p++)
        if (*p == ' ' || *p == '>' || *p == '+' || *p == '~') last = p + 1;
    while (*last == ' ') last++;
    if (!*last) return 0;

    char part[128];
    int i = 0;
    const char *p = last;
    while (*p) {
        if (i && (*p == '#' || *p == '.' || *p == '[')) {
            /* fall through to the check below */
        }
        i = 0;
        char kind = 0;
        if (*p == '#' || *p == '.') kind = *p++;
        else if (*p == '[') {
            /* [attr] or [attr="value"] */
            p++;
            char an[64], av[128];
            int k = 0;
            while (*p && *p != ']' && *p != '=' && k < (int)sizeof an - 1) an[k++] = *p++;
            an[k] = 0;
            av[0] = 0;
            if (*p == '=') {
                p++;
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    k = 0;
                    while (*p && *p != q && k < (int)sizeof av - 1) av[k++] = *p++;
                    av[k] = 0;
                    if (*p) p++;
                } else {
                    k = 0;
                    while (*p && *p != ']' && k < (int)sizeof av - 1) av[k++] = *p++;
                    av[k] = 0;
                }
            }
            if (*p == ']') p++;
            int alen = 0;
            const char *have = dom_attr_raw(node, an, &alen);
            if (!have) return 0;
            if (av[0] && !dom_attr_eq(node, an, av)) return 0;
            continue;
        }
        while (*p && *p != '#' && *p != '.' && *p != '[' &&
               i < (int)sizeof part - 1)
            part[i++] = *p++;
        part[i] = 0;
        if (!i) continue;

        if (kind == '#') { if (!dom_attr_eq(node, "id", part)) return 0; }
        else if (kind == '.') { if (!dom_has_class(node, part)) return 0; }
        else if (part[0] != '*') { if (!dom_ci_eq(dom[node].tag, part)) return 0; }
    }
    return 1;
}

/* The first element at or below `from` that the selector picks. */
static int jsd_query(int from, const char *sel) {
    for (int i = from; i < dom_n; i++)
        if (dom[i].kind == DOM_ELEM && jsd_matches(i, sel)) return i;
    return -1;
}

/* ==========================================================================
 * events
 * ========================================================================== */

#define JSD_MAX_HANDLERS 512

typedef struct {
    int    node;
    char   type[24];
    js_val fn;
} jsd_handler;

static jsd_handler jsd_handlers[JSD_MAX_HANDLERS];
static int jsd_nhandlers;

/* Timers, run by the browser's main loop. */
#define JSD_MAX_TIMERS 64
typedef struct {
    int      live;
    int      repeat;         /* setInterval */
    unsigned due, every;
    js_val   fn;
} jsd_timer;

static jsd_timer jsd_timers[JSD_MAX_TIMERS];
static int jsd_ntimers;

static js_val jsd_setTimeout(js_val self, js_val *a, int n) {
    (void)self;
    if (jsd_ntimers >= JSD_MAX_TIMERS) return js_num(-1);
    js_val fn = js_arg(a, n, 0);
    if (fn.t != JS_OBJ) return js_num(-1);
    unsigned ms = (unsigned)js_to_num(js_arg(a, n, 1));
    jsd_timer *t = &jsd_timers[jsd_ntimers];
    t->live = 1; t->repeat = 0;
    t->every = ms;
    t->due = (jsd_now_hook ? jsd_now_hook() : 0) + ms;
    t->fn = fn;
    return js_num(jsd_ntimers++);
}

static js_val jsd_setInterval(js_val self, js_val *a, int n) {
    js_val id = jsd_setTimeout(self, a, n);
    int i = (int)js_to_num(id);
    if (i >= 0) {
        jsd_timers[i].repeat = 1;
        /* An interval of nothing would run flat out; a page that asks for one
         * means "as often as you can", and ten milliseconds is that. */
        if (jsd_timers[i].every < 10) jsd_timers[i].every = 10;
    }
    return id;
}

static js_val jsd_clearTimer(js_val self, js_val *a, int n) {
    (void)self;
    int i = (int)js_to_num(js_arg(a, n, 0));
    if (i >= 0 && i < jsd_ntimers) jsd_timers[i].live = 0;
    return js_undef();
}

/* Run whatever is due. Returns 1 if anything ran. */
static int jsd_run_timers(unsigned now) {
    int ran = 0;
    for (int i = 0; i < jsd_ntimers; i++) {
        jsd_timer *t = &jsd_timers[i];
        if (!t->live || now < t->due) continue;
        if (t->repeat) t->due = now + t->every;
        else t->live = 0;
        js_call(t->fn, js_undef(), 0, 0);
        js_signal = C_NORMAL;
        ran = 1;
    }
    return ran;
}

static js_val jsd_addEventListener(js_val self, js_val *a, int n) {
    int node = jsd_node_of(self);
    js_val type = js_to_string(js_arg(a, n, 0));
    js_val fn = js_arg(a, n, 1);
    if (fn.t != JS_OBJ || jsd_nhandlers >= JSD_MAX_HANDLERS) return js_undef();
    jsd_handler *h = &jsd_handlers[jsd_nhandlers++];
    h->node = node;
    snprintf(h->type, sizeof h->type, "%s", type.u.s.p);
    h->fn = fn;
    return js_undef();
}

static js_val jsd_removeEventListener(js_val self, js_val *a, int n) {
    int node = jsd_node_of(self);
    js_val type = js_to_string(js_arg(a, n, 0));
    js_val fn = js_arg(a, n, 1);
    for (int i = 0; i < jsd_nhandlers; i++)
        if (jsd_handlers[i].node == node && fn.t == JS_OBJ &&
            jsd_handlers[i].fn.t == JS_OBJ &&
            jsd_handlers[i].fn.u.o == fn.u.o &&
            !strcmp(jsd_handlers[i].type, type.u.s.p))
            jsd_handlers[i].node = -2;      /* struck out */
    return js_undef();
}

/* Fire an event at a node and then at everything above it, which is what
 * "bubbling" amounts to and what every handler written for a list of items
 * depends on. Returns 1 if anything handled it. */
static int jsd_dispatch(int node, const char *type) {
    int fired = 0;
    for (int n = node; n >= 0; n = dom[n].parent) {
        for (int i = 0; i < jsd_nhandlers; i++) {
            if (jsd_handlers[i].node != n) continue;
            if (strcmp(jsd_handlers[i].type, type)) continue;
            js_obj *ev = js_new(JSO_PLAIN);
            js_set(ev, "type", js_str(type));
            js_set(ev, "target", jsd_element(node));
            js_set(ev, "currentTarget", jsd_element(n));
            js_def(ev, "preventDefault", jsd_nothing);
            js_def(ev, "stopPropagation", jsd_nothing);
            js_val args[1] = { js_obj_val(ev) };
            js_call(jsd_handlers[i].fn, jsd_element(n), args, 1);
            js_signal = C_NORMAL;
            fired = 1;
        }
        if (n == 0) break;
    }
    return fired;
}

/* Events that belong to the page rather than to anything in it --
 * DOMContentLoaded and load, which is where a great deal of page code
 * actually starts. */
static int jsd_fire_global(const char *type) {
    int fired = 0;
    for (int i = 0; i < jsd_nhandlers; i++) {
        if (jsd_handlers[i].node != -1) continue;
        if (strcmp(jsd_handlers[i].type, type)) continue;
        js_obj *ev = js_new(JSO_PLAIN);
        js_set(ev, "type", js_str(type));
        js_def(ev, "preventDefault", jsd_nothing);
        js_def(ev, "stopPropagation", jsd_nothing);
        js_val args[1] = { js_obj_val(ev) };
        js_call(jsd_handlers[i].fn, js_undef(), args, 1);
        js_signal = C_NORMAL;
        fired = 1;
    }
    return fired;
}

/* ==========================================================================
 * element properties
 * ========================================================================== */

static char jsd_textbuf[65536];

static js_val jsd_getAttribute(js_val self, js_val *a, int n) {
    int node = jsd_node_of(self);
    int len = 0;
    const char *v = dom_attr_raw(node, js_cstr(js_arg(a, n, 0)), &len);
    return v ? js_str_n(v, len) : js_null();
}
static js_val jsd_hasAttribute(js_val self, js_val *a, int n) {
    int len = 0;
    return js_bool(dom_attr_raw(jsd_node_of(self),
                                js_cstr(js_arg(a, n, 0)), &len) != 0);
}
static js_val jsd_setAttribute(js_val self, js_val *a, int n) {
    dom_set_attr(jsd_node_of(self), js_cstr(js_arg(a, n, 0)),
                 js_cstr(js_arg(a, n, 1)));
    dom_dirty = 1;
    return js_undef();
}
static js_val jsd_removeAttribute(js_val self, js_val *a, int n) {
    dom_set_attr(jsd_node_of(self), js_cstr(js_arg(a, n, 0)), 0);
    dom_dirty = 1;
    return js_undef();
}

static js_val jsd_querySelector(js_val self, js_val *a, int n) {
    int from = jsd_node_of(self);
    if (from < 0) from = 0;
    const char *sel = js_cstr(js_arg(a, n, 0));
    /* Search inside this element only. */
    int stop = dom_n;
    int hit = -1;
    for (int i = from + 1; i < stop; i++) {
        /* stay within the subtree */
        int p = i, inside = 0;
        for (int k = 0; k < DOM_DEPTH_MAX && p >= 0; k++, p = dom[p].parent)
            if (p == from) { inside = 1; break; }
        if (!inside && from != 0) continue;
        if (dom[i].kind == DOM_ELEM && jsd_matches(i, sel)) { hit = i; break; }
    }
    return jsd_element(hit);
}

static js_val jsd_querySelectorAll(js_val self, js_val *a, int n) {
    int from = jsd_node_of(self);
    if (from < 0) from = 0;
    const char *sel = js_cstr(js_arg(a, n, 0));
    js_obj *out = js_new_array();
    for (int i = (from ? from + 1 : 0); i < dom_n; i++) {
        if (dom[i].kind != DOM_ELEM) continue;
        if (from) {
            int p = i, inside = 0;
            for (int k = 0; k < DOM_DEPTH_MAX && p >= 0; k++, p = dom[p].parent)
                if (p == from) { inside = 1; break; }
            if (!inside) continue;
        }
        if (jsd_matches(i, sel)) js_arr_push(out, jsd_element(i));
    }
    return js_obj_val(out);
}

static js_val jsd_getElementsByTagName(js_val self, js_val *a, int n) {
    (void)self;
    const char *tag = js_cstr(js_arg(a, n, 0));
    js_obj *out = js_new_array();
    for (int i = 0; i < dom_n; i++)
        if (dom[i].kind == DOM_ELEM &&
            (tag[0] == '*' || dom_ci_eq(dom[i].tag, tag)))
            js_arr_push(out, jsd_element(i));
    return js_obj_val(out);
}
static js_val jsd_getElementsByClassName(js_val self, js_val *a, int n) {
    (void)self;
    const char *cls = js_cstr(js_arg(a, n, 0));
    js_obj *out = js_new_array();
    for (int i = 0; i < dom_n; i++)
        if (dom[i].kind == DOM_ELEM && dom_has_class(i, cls))
            js_arr_push(out, jsd_element(i));
    return js_obj_val(out);
}

static js_val jsd_appendChild(js_val self, js_val *a, int n) {
    int parent = jsd_node_of(self);
    int child = jsd_node_of(js_arg(a, n, 0));
    if (parent < 0 || child < 0) return js_undef();
    dom_append(parent, child);
    dom_dirty = 1;
    return js_arg(a, n, 0);
}
static js_val jsd_removeSelf(js_val self, js_val *a, int n) {
    (void)a; (void)n;
    dom_detach(jsd_node_of(self));
    dom_dirty = 1;
    return js_undef();
}
static js_val jsd_click(js_val self, js_val *a, int n) {
    (void)a; (void)n;
    jsd_dispatch(jsd_node_of(self), "click");
    return js_undef();
}
static js_val jsd_nothing(js_val self, js_val *a, int n) {
    (void)self; (void)a; (void)n;
    return js_undef();
}
static js_val jsd_contains(js_val self, js_val *a, int n) {
    int me = jsd_node_of(self), other = jsd_node_of(js_arg(a, n, 0));
    for (int p = other; p >= 0; p = dom[p].parent) if (p == me) return js_bool(1);
    return js_bool(0);
}
static js_val jsd_closest(js_val self, js_val *a, int n) {
    const char *sel = js_cstr(js_arg(a, n, 0));
    for (int p = jsd_node_of(self); p >= 0; p = dom[p].parent)
        if (jsd_matches(p, sel)) return jsd_element(p);
    return js_null();
}

/* --- classList -------------------------------------------------------------- */

static js_val jsd_cl_contains(js_val self, js_val *a, int n) {
    return js_bool(dom_has_class(jsd_node_of(self), js_cstr(js_arg(a, n, 0))));
}
static js_val jsd_cl_add(js_val self, js_val *a, int n) {
    int node = jsd_node_of(self);
    const char *cls = js_cstr(js_arg(a, n, 0));
    if (dom_has_class(node, cls)) return js_undef();
    int len = 0;
    const char *have = dom_attr_raw(node, "class", &len);
    char buf[512];
    snprintf(buf, sizeof buf, "%.*s%s%s", len, have ? have : "",
             (have && len) ? " " : "", cls);
    dom_set_attr(node, "class", buf);
    dom_dirty = 1;
    return js_undef();
}
static js_val jsd_cl_remove(js_val self, js_val *a, int n) {
    int node = jsd_node_of(self);
    const char *cls = js_cstr(js_arg(a, n, 0));
    int len = 0;
    const char *have = dom_attr_raw(node, "class", &len);
    if (!have) return js_undef();
    char buf[512];
    int out = 0, i = 0;
    while (i < len) {
        while (i < len && dom_isspace(have[i])) i++;
        int s = i;
        while (i < len && !dom_isspace(have[i])) i++;
        int wl = i - s;
        if (wl == (int)strlen(cls) && !memcmp(have + s, cls, (size_t)wl)) continue;
        if (out && out < (int)sizeof buf - 1) buf[out++] = ' ';
        for (int k = 0; k < wl && out < (int)sizeof buf - 1; k++) buf[out++] = have[s + k];
    }
    buf[out] = 0;
    dom_set_attr(node, "class", buf);
    dom_dirty = 1;
    return js_undef();
}
static js_val jsd_cl_toggle(js_val self, js_val *a, int n) {
    if (dom_has_class(jsd_node_of(self), js_cstr(js_arg(a, n, 0))))
        return jsd_cl_remove(self, a, n), js_bool(0);
    return jsd_cl_add(self, a, n), js_bool(1);
}

/* --- style ------------------------------------------------------------------- */

/* A style object writes back into the element's style attribute, so that the
 * next layout sees it through the ordinary cascade rather than a second path
 * that would have to agree with the first. */
static int jsd_style_set(js_obj *o, const char *name, js_val v) {
    int node = o->host;
    if (node < 0) return 0;

    /* colour -> color, backgroundColor -> background-color */
    char prop[64];
    int k = 0;
    for (int i = 0; name[i] && k < (int)sizeof prop - 2; i++) {
        if (name[i] >= 'A' && name[i] <= 'Z') {
            prop[k++] = '-';
            prop[k++] = (char)(name[i] + 32);
        } else prop[k++] = name[i];
    }
    prop[k] = 0;

    int len = 0;
    const char *have = dom_attr_raw(node, "style", &len);
    char buf[1024];
    int out = 0;

    /* copy the declarations that are not this one */
    int i = 0;
    while (have && i < len) {
        int s = i;
        while (i < len && have[i] != ';') i++;
        int dl = i - s;
        if (i < len) i++;
        int c = s;
        while (c < s + dl && have[c] != ':') c++;
        int nl = c - s;
        while (nl > 0 && dom_isspace(have[s + nl - 1])) nl--;
        int skip = 0;
        {
            int b = s;
            while (b < s + nl && dom_isspace(have[b])) b++;
            int realn = s + nl - b;
            if (realn == k && !memcmp(have + b, prop, (size_t)k)) skip = 1;
        }
        if (!skip && dl > 0) {
            for (int q = 0; q < dl && out < (int)sizeof buf - 2; q++) buf[out++] = have[s + q];
            if (out < (int)sizeof buf - 2) buf[out++] = ';';
        }
    }
    js_val sv = js_to_string(v);
    if (sv.u.s.len)
        out += snprintf(buf + out, sizeof buf - (size_t)out, "%s:%s;", prop, sv.u.s.p);
    buf[out] = 0;

    dom_set_attr(node, "style", buf);
    dom_dirty = 1;
    return 1;
}

static int jsd_style_get(js_obj *o, const char *name, js_val *out) {
    (void)name;
    if (o->host < 0) return 0;
    *out = js_str("");
    return 1;
}

/* ==========================================================================
 * the element itself
 * ========================================================================== */

/* The live state of a form control -- what has been typed into it, what is
 * ticked, which choice is showing -- belongs to the browser. The markup only
 * ever said what the control STARTED as, so answering `input.value` out of
 * the value attribute returns the page's own placeholder no matter what
 * anybody typed, and assigning to it changes an attribute nothing reads.
 *
 * These two reach the browser without jsdom knowing what a field is. Unset,
 * everything below falls back to the attributes exactly as before. */
static int (*jsd_field_read)(int node, const char *what, char *out, int max);
static int (*jsd_field_write)(int node, const char *what, const char *v);

static int jsd_is_live_prop(const char *name) {
    return !strcmp(name, "value") || !strcmp(name, "checked") ||
           !strcmp(name, "selectedIndex");
}

static int jsd_elem_get(js_obj *o, const char *name, js_val *out) {
    int node = o->host;
    if (node < 0 || node >= dom_n) return 0;

    if (!strcmp(name, "textContent") || !strcmp(name, "innerText")) {
        int n = dom_text_into(node, jsd_textbuf, (int)sizeof jsd_textbuf, 0);
        jsd_textbuf[n] = 0;
        *out = js_str_n(jsd_textbuf, n);
        return 1;
    }
    if (!strcmp(name, "innerHTML")) {
        int n = dom_html_into(node, jsd_textbuf, (int)sizeof jsd_textbuf, 0, 0);
        jsd_textbuf[n] = 0;
        *out = js_str_n(jsd_textbuf, n);
        return 1;
    }
    if (!strcmp(name, "tagName") || !strcmp(name, "nodeName")) {
        char up[DOM_TAG_MAX];
        int i = 0;
        for (; dom[node].tag[i] && i < DOM_TAG_MAX - 1; i++) {
            char c = dom[node].tag[i];
            up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        up[i] = 0;
        *out = js_str(up);
        return 1;
    }
    if (!strcmp(name, "localName")) { *out = js_str(dom[node].tag); return 1; }

    if (jsd_field_read && jsd_is_live_prop(name)) {
        char buf[512];
        if (jsd_field_read(node, name, buf, (int)sizeof buf) >= 0) {
            if (!strcmp(name, "checked"))            *out = js_bool(buf[0] == '1');
            else if (!strcmp(name, "selectedIndex")) *out = js_num(atoi(buf));
            else                                     *out = js_str(buf);
            return 1;
        }
    }

    if (!strcmp(name, "id") || !strcmp(name, "href") || !strcmp(name, "src") ||
        !strcmp(name, "alt") || !strcmp(name, "title") || !strcmp(name, "type") ||
        !strcmp(name, "name") || !strcmp(name, "value") ||
        !strcmp(name, "placeholder")) {
        int len = 0;
        const char *v = dom_attr_raw(node, name, &len);
        *out = v ? js_str_n(v, len) : js_str("");
        return 1;
    }
    if (!strcmp(name, "className")) {
        int len = 0;
        const char *v = dom_attr_raw(node, "class", &len);
        *out = v ? js_str_n(v, len) : js_str("");
        return 1;
    }
    if (!strcmp(name, "classList")) {
        js_obj *cl = js_new(JSO_PLAIN);
        cl->host = node;
        cl->hget = 0; cl->hset = 0;
        js_def(cl, "contains", jsd_cl_contains);
        js_def(cl, "add", jsd_cl_add);
        js_def(cl, "remove", jsd_cl_remove);
        js_def(cl, "toggle", jsd_cl_toggle);
        *out = js_obj_val(cl);
        return 1;
    }
    if (!strcmp(name, "style")) {
        js_obj *st = js_new(JSO_PLAIN);
        st->host = node;
        st->hget = jsd_style_get;
        st->hset = jsd_style_set;
        *out = js_obj_val(st);
        return 1;
    }
    if (!strcmp(name, "parentElement") || !strcmp(name, "parentNode")) {
        *out = jsd_element(dom[node].parent > 0 ? dom[node].parent : -1);
        return 1;
    }
    if (!strcmp(name, "firstElementChild") || !strcmp(name, "firstChild")) {
        int c = dom[node].first;
        while (c >= 0 && dom[c].kind != DOM_ELEM) c = dom[c].next;
        *out = jsd_element(c);
        return 1;
    }
    if (!strcmp(name, "nextElementSibling")) {
        int c = dom[node].next;
        while (c >= 0 && dom[c].kind != DOM_ELEM) c = dom[c].next;
        *out = jsd_element(c);
        return 1;
    }
    if (!strcmp(name, "children") || !strcmp(name, "childNodes")) {
        js_obj *arr = js_new_array();
        for (int c = dom[node].first; c >= 0; c = dom[c].next)
            if (dom[c].kind == DOM_ELEM) js_arr_push(arr, jsd_element(c));
        *out = js_obj_val(arr);
        return 1;
    }
    if (!strcmp(name, "childElementCount")) {
        int k = 0;
        for (int c = dom[node].first; c >= 0; c = dom[c].next)
            if (dom[c].kind == DOM_ELEM) k++;
        *out = js_num(k);
        return 1;
    }
    if (!strcmp(name, "nodeType")) {
        *out = js_num(dom[node].kind == DOM_ELEM ? 1 : 3);
        return 1;
    }
    if (!strcmp(name, "checked") || !strcmp(name, "disabled") ||
        !strcmp(name, "hidden")) {
        int len = 0;
        *out = js_bool(dom_attr_raw(node, name, &len) != 0);
        return 1;
    }

    /* methods */
    if (!strcmp(name, "getAttribute"))    { *out = js_native_fn(name, jsd_getAttribute); return 1; }
    if (!strcmp(name, "setAttribute"))    { *out = js_native_fn(name, jsd_setAttribute); return 1; }
    if (!strcmp(name, "hasAttribute"))    { *out = js_native_fn(name, jsd_hasAttribute); return 1; }
    if (!strcmp(name, "removeAttribute")) { *out = js_native_fn(name, jsd_removeAttribute); return 1; }
    if (!strcmp(name, "querySelector"))   { *out = js_native_fn(name, jsd_querySelector); return 1; }
    if (!strcmp(name, "querySelectorAll")){ *out = js_native_fn(name, jsd_querySelectorAll); return 1; }
    if (!strcmp(name, "getElementsByTagName"))   { *out = js_native_fn(name, jsd_getElementsByTagName); return 1; }
    if (!strcmp(name, "getElementsByClassName")) { *out = js_native_fn(name, jsd_getElementsByClassName); return 1; }
    if (!strcmp(name, "addEventListener"))    { *out = js_native_fn(name, jsd_addEventListener); return 1; }
    if (!strcmp(name, "removeEventListener")) { *out = js_native_fn(name, jsd_removeEventListener); return 1; }
    if (!strcmp(name, "appendChild"))     { *out = js_native_fn(name, jsd_appendChild); return 1; }
    if (!strcmp(name, "remove"))          { *out = js_native_fn(name, jsd_removeSelf); return 1; }
    if (!strcmp(name, "click"))           { *out = js_native_fn(name, jsd_click); return 1; }
    if (!strcmp(name, "contains"))        { *out = js_native_fn(name, jsd_contains); return 1; }
    if (!strcmp(name, "closest"))         { *out = js_native_fn(name, jsd_closest); return 1; }
    if (!strcmp(name, "focus") || !strcmp(name, "blur") ||
        !strcmp(name, "scrollIntoView"))  { *out = js_native_fn(name, jsd_nothing); return 1; }

    return 0;
}

static int jsd_elem_set(js_obj *o, const char *name, js_val v) {
    int node = o->host;
    if (node < 0 || node >= dom_n) return 0;

    if (!strcmp(name, "textContent") || !strcmp(name, "innerText")) {
        js_val s = js_to_string(v);
        dom_set_text(node, s.u.s.p, s.u.s.len);
        dom_dirty = 1;
        return 1;
    }
    if (!strcmp(name, "innerHTML")) {
        js_val s = js_to_string(v);
        dom_set_html(node, s.u.s.p, s.u.s.len);
        dom_dirty = 1;
        return 1;
    }
    if (!strcmp(name, "className")) {
        dom_set_attr(node, "class", js_cstr(v));
        dom_dirty = 1;
        return 1;
    }
    if (jsd_field_write && jsd_is_live_prop(name)) {
        js_val s = js_to_string(v);
        if (jsd_field_write(node, name, s.u.s.p)) return 1;
    }

    if (!strcmp(name, "id") || !strcmp(name, "href") || !strcmp(name, "src") ||
        !strcmp(name, "alt") || !strcmp(name, "title") ||
        !strcmp(name, "value") || !strcmp(name, "type")) {
        dom_set_attr(node, name, js_cstr(v));
        dom_dirty = 1;
        return 1;
    }
    /* onclick = function, which older pages use instead of a listener */
    if (!strncmp(name, "on", 2) && v.t == JS_OBJ) {
        if (jsd_nhandlers < JSD_MAX_HANDLERS) {
            jsd_handler *h = &jsd_handlers[jsd_nhandlers++];
            h->node = node;
            snprintf(h->type, sizeof h->type, "%s", name + 2);
            h->fn = v;
        }
        return 1;
    }
    return 0;
}

/* ==========================================================================
 * document and window
 * ========================================================================== */

static js_val jsd_getElementById(js_val self, js_val *a, int n) {
    (void)self;
    return jsd_element(dom_find_id(js_cstr(js_arg(a, n, 0))));
}

static js_val jsd_createElement(js_val self, js_val *a, int n) {
    (void)self;
    int node = dom_create(js_cstr(js_arg(a, n, 0)));
    return jsd_element(node);
}
static js_val jsd_createTextNode(js_val self, js_val *a, int n) {
    (void)self;
    js_val s = js_to_string(js_arg(a, n, 0));
    int node = dom_create("span");
    if (node >= 0) dom_set_text(node, s.u.s.p, s.u.s.len);
    return jsd_element(node);
}

static js_val jsd_alert(js_val self, js_val *a, int n) {
    return jsg_log(self, a, n);
}

static int jsd_doc_get(js_obj *o, const char *name, js_val *out) {
    (void)o;
    if (!strcmp(name, "body")) {
        int b = 0;
        for (int i = 0; i < dom_n; i++)
            if (dom[i].kind == DOM_ELEM && dom_ci_eq(dom[i].tag, "body")) { b = i; break; }
        *out = jsd_element(b ? b : dom_root);
        return 1;
    }
    if (!strcmp(name, "head")) {
        for (int i = 0; i < dom_n; i++)
            if (dom[i].kind == DOM_ELEM && dom_ci_eq(dom[i].tag, "head")) {
                *out = jsd_element(i); return 1;
            }
        *out = jsd_element(dom_root);
        return 1;
    }
    if (!strcmp(name, "documentElement")) { *out = jsd_element(dom_root); return 1; }
    if (!strcmp(name, "title")) {
        for (int i = 0; i < dom_n; i++)
            if (dom[i].kind == DOM_ELEM && dom_ci_eq(dom[i].tag, "title")) {
                int k = dom_text_into(i, jsd_textbuf, (int)sizeof jsd_textbuf, 0);
                jsd_textbuf[k] = 0;
                *out = js_str_n(jsd_textbuf, k);
                return 1;
            }
        *out = js_str("");
        return 1;
    }
    if (!strcmp(name, "readyState")) { *out = js_str("complete"); return 1; }
    return 0;
}

static void jsd_setup(const char *page_url) {
    jsd_nwrap = 0;
    jsd_nhandlers = 0;
    jsd_ntimers = 0;
    dom_dirty = 0;

    js_obj *doc = js_new(JSO_PLAIN);
    doc->host = -1;
    doc->hget = jsd_doc_get;
    js_def(doc, "getElementById", jsd_getElementById);
    js_def(doc, "querySelector", jsd_querySelector);
    js_def(doc, "querySelectorAll", jsd_querySelectorAll);
    js_def(doc, "getElementsByTagName", jsd_getElementsByTagName);
    js_def(doc, "getElementsByClassName", jsd_getElementsByClassName);
    js_def(doc, "createElement", jsd_createElement);
    js_def(doc, "createTextNode", jsd_createTextNode);
    js_def(doc, "addEventListener", jsd_addEventListener);
    js_def(doc, "removeEventListener", jsd_removeEventListener);
    js_set(js_globals, "document", js_obj_val(doc));

    js_obj *loc = js_new(JSO_PLAIN);
    js_set(loc, "href", js_str(page_url ? page_url : ""));
    js_set(js_globals, "location", js_obj_val(loc));

    js_obj *nav = js_new(JSO_PLAIN);
    js_set(nav, "userAgent", js_str("xyuOS"));
    js_set(nav, "language", js_str("en"));
    js_set(js_globals, "navigator", js_obj_val(nav));

    js_set(js_globals, "setTimeout", js_native_fn("setTimeout", jsd_setTimeout));
    js_set(js_globals, "setInterval", js_native_fn("setInterval", jsd_setInterval));
    js_set(js_globals, "clearTimeout", js_native_fn("clearTimeout", jsd_clearTimer));
    js_set(js_globals, "clearInterval", js_native_fn("clearInterval", jsd_clearTimer));
    js_set(js_globals, "requestAnimationFrame",
           js_native_fn("requestAnimationFrame", jsd_setTimeout));
    js_set(js_globals, "alert", js_native_fn("alert", jsd_alert));

    /* window is the global object, and scripts expect both to work. */
    js_obj *win = js_new(JSO_PLAIN);
    js_set(win, "document", *js_find(js_globals, "document"));
    js_set(win, "location", *js_find(js_globals, "location"));
    js_set(win, "navigator", *js_find(js_globals, "navigator"));
    js_set(win, "innerWidth", js_num(960));
    js_set(win, "innerHeight", js_num(600));
    js_def(win, "addEventListener", jsd_addEventListener);
    js_def(win, "removeEventListener", jsd_removeEventListener);
    js_def(win, "scrollTo", jsd_nothing);
    js_set(win, "setTimeout", *js_find(js_globals, "setTimeout"));
    js_set(win, "setInterval", *js_find(js_globals, "setInterval"));
    js_set(win, "alert", *js_find(js_globals, "alert"));
    js_set(js_globals, "window", js_obj_val(win));
    js_set(js_globals, "self", js_obj_val(win));
    js_set(js_globals, "globalThis", js_obj_val(win));
}

#endif /* JSDOM_H */
