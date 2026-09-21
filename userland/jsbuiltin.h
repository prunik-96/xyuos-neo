#ifndef JSBUILTIN_H
#define JSBUILTIN_H

/* The standard library, as far as it goes.
 *
 * Included from the middle of js.h, where the value machinery exists and the
 * interpreter does not yet. These are the methods real scripts reach for --
 * the string and array ones above all, since most page code is turning one
 * list of things into another and putting text somewhere.
 *
 * Methods are returned as plain native functions. The interpreter already
 * passes the object a method was read from as `self`, so nothing has to be
 * bound: "abc".toUpperCase and s.toUpperCase are the same function value, and
 * both know what they were called on.
 */

/* Where console.log goes. The browser points this at its own log. */
static void (*js_console_sink)(const char *line);

static js_val js_native_fn(const char *name, js_native fn) {
    js_obj *o = js_new(JSO_NATIVE);
    if (!o) return js_undef();
    o->native = fn;
    o->cname = name;
    return js_obj_val(o);
}

static void js_def(js_obj *o, const char *name, js_native fn) {
    js_set(o, name, js_native_fn(name, fn));
}

/* A string value from a buffer we are about to reuse. */
static js_val js_sv(const char *p, int n) { return js_str_n(p, n); }

/* ==========================================================================
 * strings
 * ========================================================================== */

static int js_index_of(js_val hay, js_val needle, int from) {
    int hl = hay.u.s.len, nl = needle.u.s.len;
    if (nl == 0) return from <= hl ? from : hl;
    for (int i = from < 0 ? 0 : from; i + nl <= hl; i++)
        if (!memcmp(hay.u.s.p + i, needle.u.s.p, (size_t)nl)) return i;
    return -1;
}

static js_val jsb_charAt(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    int i = (int)js_to_num(js_arg(a, n, 0));
    if (i < 0 || i >= v.u.s.len) return js_str("");
    return js_sv(v.u.s.p + i, 1);
}
static js_val jsb_charCodeAt(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    int i = (int)js_to_num(js_arg(a, n, 0));
    if (i < 0 || i >= v.u.s.len) return js_num(0.0 / 0.0);
    return js_num((unsigned char)v.u.s.p[i]);
}
static js_val jsb_indexOf_s(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s), w = js_to_string(js_arg(a, n, 0));
    int from = n > 1 ? (int)js_to_num(a[1]) : 0;
    return js_num(js_index_of(v, w, from));
}
static js_val jsb_lastIndexOf(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s), w = js_to_string(js_arg(a, n, 0));
    int best = -1, at = 0;
    for (;;) {
        int i = js_index_of(v, w, at);
        if (i < 0) break;
        best = i; at = i + 1;
    }
    return js_num(best);
}
static void js_clamp_range(int len, double *ps, double *pe) {
    double s = *ps, e = *pe;
    if (s < 0) s += len;
    if (e < 0) e += len;
    if (s < 0) s = 0;
    if (e > len) e = len;
    if (e < s) e = s;
    *ps = s; *pe = e;
}
static js_val jsb_slice_s(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    double st = n > 0 ? js_to_num(a[0]) : 0;
    double en = (n > 1 && a[1].t != JS_UNDEF) ? js_to_num(a[1]) : v.u.s.len;
    js_clamp_range(v.u.s.len, &st, &en);
    return js_sv(v.u.s.p + (int)st, (int)(en - st));
}
static js_val jsb_substring(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    double st = n > 0 ? js_to_num(a[0]) : 0;
    double en = (n > 1 && a[1].t != JS_UNDEF) ? js_to_num(a[1]) : v.u.s.len;
    if (st < 0) st = 0;
    if (en < 0) en = 0;
    if (st > v.u.s.len) st = v.u.s.len;
    if (en > v.u.s.len) en = v.u.s.len;
    if (st > en) { double t = st; st = en; en = t; }
    return js_sv(v.u.s.p + (int)st, (int)(en - st));
}
static js_val jsb_substr(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    double st = n > 0 ? js_to_num(a[0]) : 0;
    if (st < 0) st += v.u.s.len;
    if (st < 0) st = 0;
    double cnt = (n > 1 && a[1].t != JS_UNDEF) ? js_to_num(a[1]) : v.u.s.len - st;
    if (st + cnt > v.u.s.len) cnt = v.u.s.len - st;
    if (cnt < 0) cnt = 0;
    return js_sv(v.u.s.p + (int)st, (int)cnt);
}
static js_val jsb_upper(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_val v = js_to_string(s);
    js_val r = js_str_n(v.u.s.p, v.u.s.len);
    char *p = (char *)r.u.s.p;
    for (int i = 0; i < r.u.s.len; i++)
        if (p[i] >= 'a' && p[i] <= 'z') p[i] = (char)(p[i] - 32);
    return r;
}
static js_val jsb_lower(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_val v = js_to_string(s);
    js_val r = js_str_n(v.u.s.p, v.u.s.len);
    char *p = (char *)r.u.s.p;
    for (int i = 0; i < r.u.s.len; i++)
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] = (char)(p[i] + 32);
    return r;
}
static int js_ws(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static js_val jsb_trim(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_val v = js_to_string(s);
    int i = 0, j = v.u.s.len;
    while (i < j && js_ws((unsigned char)v.u.s.p[i])) i++;
    while (j > i && js_ws((unsigned char)v.u.s.p[j-1])) j--;
    return js_sv(v.u.s.p + i, j - i);
}
static js_val jsb_split(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    js_obj *out = js_new_array();
    if (n == 0 || a[0].t == JS_UNDEF) { js_arr_push(out, v); return js_obj_val(out); }
    js_val sep = js_to_string(a[0]);
    if (sep.u.s.len == 0) {
        for (int i = 0; i < v.u.s.len; i++) js_arr_push(out, js_sv(v.u.s.p + i, 1));
        return js_obj_val(out);
    }
    int at = 0;
    for (;;) {
        int i = js_index_of(v, sep, at);
        if (i < 0) break;
        js_arr_push(out, js_sv(v.u.s.p + at, i - at));
        at = i + sep.u.s.len;
    }
    js_arr_push(out, js_sv(v.u.s.p + at, v.u.s.len - at));
    return js_obj_val(out);
}
static js_val js_replace_impl(js_val s, js_val *a, int n, int all) {
    js_val v = js_to_string(s);
    if (n < 2) return v;
    js_val from = js_to_string(a[0]);
    js_val to = js_to_string(a[1]);
    if (from.u.s.len == 0) return v;

    static char buf[16384];
    int len = 0, at = 0;
    for (;;) {
        int i = js_index_of(v, from, at);
        if (i < 0) break;
        for (int k = at; k < i && len < (int)sizeof buf; k++) buf[len++] = v.u.s.p[k];
        for (int k = 0; k < to.u.s.len && len < (int)sizeof buf; k++) buf[len++] = to.u.s.p[k];
        at = i + from.u.s.len;
        if (!all) break;
    }
    for (int k = at; k < v.u.s.len && len < (int)sizeof buf; k++) buf[len++] = v.u.s.p[k];
    return js_sv(buf, len);
}
static js_val jsb_replace(js_val s, js_val *a, int n)    { return js_replace_impl(s, a, n, 0); }
static js_val jsb_replaceAll(js_val s, js_val *a, int n) { return js_replace_impl(s, a, n, 1); }

static js_val jsb_includes_s(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s), w = js_to_string(js_arg(a, n, 0));
    return js_bool(js_index_of(v, w, 0) >= 0);
}
static js_val jsb_startsWith(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s), w = js_to_string(js_arg(a, n, 0));
    int off = n > 1 ? (int)js_to_num(a[1]) : 0;
    if (off < 0 || off + w.u.s.len > v.u.s.len) return js_bool(0);
    return js_bool(!memcmp(v.u.s.p + off, w.u.s.p, (size_t)w.u.s.len));
}
static js_val jsb_endsWith(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s), w = js_to_string(js_arg(a, n, 0));
    if (w.u.s.len > v.u.s.len) return js_bool(0);
    return js_bool(!memcmp(v.u.s.p + v.u.s.len - w.u.s.len, w.u.s.p, (size_t)w.u.s.len));
}
static js_val jsb_repeat(js_val s, js_val *a, int n) {
    js_val v = js_to_string(s);
    int k = (int)js_to_num(js_arg(a, n, 0));
    if (k < 0) k = 0;
    long total = (long)k * v.u.s.len;
    if (total > 1 << 20) { js_throw_str("repeat asked for too much"); return js_str(""); }
    char *p = (char *)js_alloc(total + 1);
    if (!p) return js_str("");
    for (int i = 0; i < k; i++) memcpy(p + (long)i * v.u.s.len, v.u.s.p, (size_t)v.u.s.len);
    p[total] = 0;
    js_val r; r.t = JS_STR; r.u.s.p = p; r.u.s.len = (int)total;
    return r;
}
static js_val js_pad(js_val s, js_val *a, int n, int start) {
    js_val v = js_to_string(s);
    int want = (int)js_to_num(js_arg(a, n, 0));
    js_val fill = n > 1 ? js_to_string(a[1]) : js_str(" ");
    if (want <= v.u.s.len || fill.u.s.len == 0) return v;
    int padlen = want - v.u.s.len;
    char *p = (char *)js_alloc(want + 1);
    if (!p) return v;
    for (int i = 0; i < padlen; i++) p[i] = fill.u.s.p[i % fill.u.s.len];
    if (start) memcpy(p + padlen, v.u.s.p, (size_t)v.u.s.len);
    else { memcpy(p, v.u.s.p, (size_t)v.u.s.len);
           for (int i = 0; i < padlen; i++) p[v.u.s.len + i] = fill.u.s.p[i % fill.u.s.len]; }
    p[want] = 0;
    js_val r; r.t = JS_STR; r.u.s.p = p; r.u.s.len = want;
    return r;
}
static js_val jsb_padStart(js_val s, js_val *a, int n) { return js_pad(s, a, n, 1); }
static js_val jsb_padEnd(js_val s, js_val *a, int n)   { return js_pad(s, a, n, 0); }
static js_val jsb_concat_s(js_val s, js_val *a, int n) {
    js_val r = js_to_string(s);
    for (int i = 0; i < n; i++) r = js_binop("+", r, a[i]);
    return r;
}
static js_val jsb_tostring(js_val s, js_val *a, int n) { (void)a; (void)n; return js_to_string(s); }

static js_val js_string_prop(js_val base, const char *name) {
    if (!strcmp(name, "length")) return js_num(base.u.s.len);
    if (!strcmp(name, "charAt"))       return js_native_fn(name, jsb_charAt);
    if (!strcmp(name, "charCodeAt") ||
        !strcmp(name, "codePointAt"))  return js_native_fn(name, jsb_charCodeAt);
    if (!strcmp(name, "indexOf"))      return js_native_fn(name, jsb_indexOf_s);
    if (!strcmp(name, "lastIndexOf"))  return js_native_fn(name, jsb_lastIndexOf);
    if (!strcmp(name, "slice"))        return js_native_fn(name, jsb_slice_s);
    if (!strcmp(name, "substring"))    return js_native_fn(name, jsb_substring);
    if (!strcmp(name, "substr"))       return js_native_fn(name, jsb_substr);
    if (!strcmp(name, "toUpperCase"))  return js_native_fn(name, jsb_upper);
    if (!strcmp(name, "toLowerCase"))  return js_native_fn(name, jsb_lower);
    if (!strcmp(name, "trim"))         return js_native_fn(name, jsb_trim);
    if (!strcmp(name, "split"))        return js_native_fn(name, jsb_split);
    if (!strcmp(name, "replace"))      return js_native_fn(name, jsb_replace);
    if (!strcmp(name, "replaceAll"))   return js_native_fn(name, jsb_replaceAll);
    if (!strcmp(name, "includes"))     return js_native_fn(name, jsb_includes_s);
    if (!strcmp(name, "startsWith"))   return js_native_fn(name, jsb_startsWith);
    if (!strcmp(name, "endsWith"))     return js_native_fn(name, jsb_endsWith);
    if (!strcmp(name, "repeat"))       return js_native_fn(name, jsb_repeat);
    if (!strcmp(name, "padStart"))     return js_native_fn(name, jsb_padStart);
    if (!strcmp(name, "padEnd"))       return js_native_fn(name, jsb_padEnd);
    if (!strcmp(name, "concat"))       return js_native_fn(name, jsb_concat_s);
    if (!strcmp(name, "at"))           return js_native_fn(name, jsb_charAt);
    if (!strcmp(name, "toString") ||
        !strcmp(name, "valueOf") ||
        !strcmp(name, "trimStart") ||
        !strcmp(name, "trimEnd") ||
        !strcmp(name, "normalize"))    return js_native_fn(name, jsb_tostring);
    /* "0" and friends index the string. */
    if (name[0] >= '0' && name[0] <= '9') {
        int i = atoi(name);
        if (i >= 0 && i < base.u.s.len) return js_sv(base.u.s.p + i, 1);
    }
    return js_undef();
}

/* ==========================================================================
 * numbers
 * ========================================================================== */

static js_val jsb_toFixed(js_val s, js_val *a, int n) {
    int d = (int)js_to_num(js_arg(a, n, 0));
    if (d < 0) d = 0;
    if (d > 20) d = 20;
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", d, js_to_num(s));
    return js_str(buf);
}
static js_val jsb_num_tostring(js_val s, js_val *a, int n) {
    if (n > 0) {
        int radix = (int)js_to_num(a[0]);
        if (radix >= 2 && radix <= 36 && radix != 10) {
            long v = (long)js_to_num(s);
            char buf[80];
            int i = (int)sizeof buf - 1, neg = v < 0;
            buf[i] = 0;
            if (neg) v = -v;
            if (!v) buf[--i] = '0';
            while (v && i > 0) {
                int d = (int)(v % radix);
                buf[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v /= radix;
            }
            if (neg && i > 0) buf[--i] = '-';
            return js_str(buf + i);
        }
    }
    return js_to_string(s);
}
static js_val js_number_prop(js_val base, const char *name) {
    (void)base;
    if (!strcmp(name, "toFixed"))   return js_native_fn(name, jsb_toFixed);
    if (!strcmp(name, "toString"))  return js_native_fn(name, jsb_num_tostring);
    if (!strcmp(name, "valueOf") ||
        !strcmp(name, "toPrecision")) return js_native_fn(name, jsb_tostring);
    return js_undef();
}

/* ==========================================================================
 * arrays
 * ========================================================================== */

static js_obj *js_arr(js_val v) {
    return (v.t == JS_OBJ && v.u.o && v.u.o->kind == JSO_ARRAY) ? v.u.o : 0;
}

static js_val jsb_push(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_num(0);
    for (int i = 0; i < n; i++) js_arr_push(o, a[i]);
    return js_num(o->nelems);
}
static js_val jsb_pop(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_obj *o = js_arr(s);
    if (!o || !o->nelems) return js_undef();
    return o->elems[--o->nelems];
}
static js_val jsb_shift(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_obj *o = js_arr(s);
    if (!o || !o->nelems) return js_undef();
    js_val first = o->elems[0];
    for (int i = 1; i < o->nelems; i++) o->elems[i-1] = o->elems[i];
    o->nelems--;
    return first;
}
static js_val jsb_unshift(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_num(0);
    for (int i = 0; i < n; i++) js_arr_push(o, js_undef());
    for (int i = o->nelems - 1; i >= n; i--) o->elems[i] = o->elems[i - n];
    for (int i = 0; i < n; i++) o->elems[i] = a[i];
    return js_num(o->nelems);
}
static js_val jsb_slice_a(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    js_obj *out = js_new_array();
    if (!o) return js_obj_val(out);
    double st = n > 0 ? js_to_num(a[0]) : 0;
    double en = (n > 1 && a[1].t != JS_UNDEF) ? js_to_num(a[1]) : o->nelems;
    js_clamp_range(o->nelems, &st, &en);
    for (int i = (int)st; i < (int)en; i++) js_arr_push(out, o->elems[i]);
    return js_obj_val(out);
}
static js_val jsb_splice(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    js_obj *out = js_new_array();
    if (!o) return js_obj_val(out);
    int start = n > 0 ? (int)js_to_num(a[0]) : 0;
    if (start < 0) start += o->nelems;
    if (start < 0) start = 0;
    if (start > o->nelems) start = o->nelems;
    int del = n > 1 ? (int)js_to_num(a[1]) : o->nelems - start;
    if (del < 0) del = 0;
    if (start + del > o->nelems) del = o->nelems - start;
    for (int i = 0; i < del; i++) js_arr_push(out, o->elems[start + i]);

    int ins = n > 2 ? n - 2 : 0;
    int newlen = o->nelems - del + ins;
    while (o->ecap < newlen) js_arr_push(o, js_undef());
    if (o->nelems < newlen) o->nelems = newlen;
    /* shift the tail */
    if (ins != del) {
        if (ins > del)
            for (int i = newlen - 1; i >= start + ins; i--)
                o->elems[i] = o->elems[i - (ins - del)];
        else
            for (int i = start + ins; i < newlen; i++)
                o->elems[i] = o->elems[i + (del - ins)];
    }
    for (int i = 0; i < ins; i++) o->elems[start + i] = a[2 + i];
    o->nelems = newlen;
    return js_obj_val(out);
}
static js_val jsb_indexOf_a(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_num(-1);
    js_val want = js_arg(a, n, 0);
    for (int i = 0; i < o->nelems; i++)
        if (js_strict_eq(o->elems[i], want)) return js_num(i);
    return js_num(-1);
}
static js_val jsb_includes_a(js_val s, js_val *a, int n) {
    return js_bool(js_to_num(jsb_indexOf_a(s, a, n)) >= 0);
}
static js_val jsb_join(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_str("");
    js_val sep = n > 0 && a[0].t != JS_UNDEF ? js_to_string(a[0]) : js_str(",");
    static char buf[16384];
    int len = 0;
    for (int i = 0; i < o->nelems; i++) {
        if (i) for (int k = 0; k < sep.u.s.len && len < (int)sizeof buf; k++)
                   buf[len++] = sep.u.s.p[k];
        js_val e = o->elems[i];
        if (e.t == JS_UNDEF || e.t == JS_NULL) continue;
        js_val t = js_to_string(e);
        for (int k = 0; k < t.u.s.len && len < (int)sizeof buf; k++) buf[len++] = t.u.s.p[k];
    }
    return js_sv(buf, len);
}
static js_val jsb_concat_a(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    js_obj *out = js_new_array();
    if (o) for (int i = 0; i < o->nelems; i++) js_arr_push(out, o->elems[i]);
    for (int i = 0; i < n; i++) {
        js_obj *b = js_arr(a[i]);
        if (b) for (int k = 0; k < b->nelems; k++) js_arr_push(out, b->elems[k]);
        else js_arr_push(out, a[i]);
    }
    return js_obj_val(out);
}
static js_val jsb_reverse(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_obj *o = js_arr(s);
    if (!o) return s;
    for (int i = 0, j = o->nelems - 1; i < j; i++, j--) {
        js_val t = o->elems[i]; o->elems[i] = o->elems[j]; o->elems[j] = t;
    }
    return s;
}
static js_val jsb_sort(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return s;
    js_val cmp = js_arg(a, n, 0);
    int have = (cmp.t == JS_OBJ);
    /* Insertion sort: arrays on a page are short, and it is stable, which
     * the language requires. */
    for (int i = 1; i < o->nelems; i++) {
        js_val v = o->elems[i];
        int j = i - 1;
        while (j >= 0) {
            int gt;
            if (have) {
                js_val args[2] = { o->elems[j], v };
                gt = js_to_num(js_call(cmp, js_undef(), args, 2)) > 0;
            } else {
                js_val x = js_to_string(o->elems[j]), y = js_to_string(v);
                int m = x.u.s.len < y.u.s.len ? x.u.s.len : y.u.s.len;
                int c = memcmp(x.u.s.p, y.u.s.p, (size_t)m);
                if (!c) c = x.u.s.len - y.u.s.len;
                gt = c > 0;
            }
            if (!gt) break;
            o->elems[j + 1] = o->elems[j];
            j--;
        }
        o->elems[j + 1] = v;
    }
    return s;
}

/* The iteration methods all share the same shape. */
enum { IT_FOREACH, IT_MAP, IT_FILTER, IT_FIND, IT_FINDIX, IT_SOME, IT_EVERY };

static js_val js_iterate(js_val s, js_val *a, int n, int kind) {
    js_obj *o = js_arr(s);
    js_val fn = js_arg(a, n, 0);
    js_obj *out = (kind == IT_MAP || kind == IT_FILTER) ? js_new_array() : 0;
    if (!o) return out ? js_obj_val(out) : js_undef();

    for (int i = 0; i < o->nelems; i++) {
        js_val args[3] = { o->elems[i], js_num(i), s };
        js_val r = js_call(fn, js_arg(a, n, 1), args, 3);
        if (js_signal) break;
        switch (kind) {
        case IT_MAP:    js_arr_push(out, r); break;
        case IT_FILTER: if (js_truthy(r)) js_arr_push(out, o->elems[i]); break;
        case IT_FIND:   if (js_truthy(r)) return o->elems[i]; break;
        case IT_FINDIX: if (js_truthy(r)) return js_num(i); break;
        case IT_SOME:   if (js_truthy(r)) return js_bool(1); break;
        case IT_EVERY:  if (!js_truthy(r)) return js_bool(0); break;
        default: break;
        }
    }
    switch (kind) {
    case IT_MAP: case IT_FILTER: return js_obj_val(out);
    case IT_FIND:   return js_undef();
    case IT_FINDIX: return js_num(-1);
    case IT_SOME:   return js_bool(0);
    case IT_EVERY:  return js_bool(1);
    default: return js_undef();
    }
}
static js_val jsb_forEach(js_val s, js_val *a, int n) { return js_iterate(s, a, n, IT_FOREACH); }
static js_val jsb_map(js_val s, js_val *a, int n)     { return js_iterate(s, a, n, IT_MAP); }
static js_val jsb_filter(js_val s, js_val *a, int n)  { return js_iterate(s, a, n, IT_FILTER); }
static js_val jsb_find(js_val s, js_val *a, int n)    { return js_iterate(s, a, n, IT_FIND); }
static js_val jsb_findIndex(js_val s, js_val *a, int n){ return js_iterate(s, a, n, IT_FINDIX); }
static js_val jsb_some(js_val s, js_val *a, int n)    { return js_iterate(s, a, n, IT_SOME); }
static js_val jsb_every(js_val s, js_val *a, int n)   { return js_iterate(s, a, n, IT_EVERY); }

static js_val jsb_reduce(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_undef();
    js_val fn = js_arg(a, n, 0);
    int i = 0;
    js_val acc;
    if (n > 1) acc = a[1];
    else {
        if (!o->nelems) { js_throw_str("reduce of an empty array"); return js_undef(); }
        acc = o->elems[0]; i = 1;
    }
    for (; i < o->nelems; i++) {
        js_val args[4] = { acc, o->elems[i], js_num(i), s };
        acc = js_call(fn, js_undef(), args, 4);
        if (js_signal) break;
    }
    return acc;
}
static js_val jsb_fill(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return s;
    js_val v = js_arg(a, n, 0);
    for (int i = 0; i < o->nelems; i++) o->elems[i] = v;
    return s;
}
static js_val jsb_flat(js_val s, js_val *a, int n) {
    (void)a; (void)n;
    js_obj *o = js_arr(s);
    js_obj *out = js_new_array();
    if (o) for (int i = 0; i < o->nelems; i++) {
        js_obj *b = js_arr(o->elems[i]);
        if (b) for (int k = 0; k < b->nelems; k++) js_arr_push(out, b->elems[k]);
        else js_arr_push(out, o->elems[i]);
    }
    return js_obj_val(out);
}
static js_val jsb_at(js_val s, js_val *a, int n) {
    js_obj *o = js_arr(s);
    if (!o) return js_undef();
    int i = (int)js_to_num(js_arg(a, n, 0));
    if (i < 0) i += o->nelems;
    return (i >= 0 && i < o->nelems) ? o->elems[i] : js_undef();
}

static js_val js_array_prop(js_val base, const char *name) {
    (void)base;
    if (!strcmp(name, "push"))       return js_native_fn(name, jsb_push);
    if (!strcmp(name, "pop"))        return js_native_fn(name, jsb_pop);
    if (!strcmp(name, "shift"))      return js_native_fn(name, jsb_shift);
    if (!strcmp(name, "unshift"))    return js_native_fn(name, jsb_unshift);
    if (!strcmp(name, "slice"))      return js_native_fn(name, jsb_slice_a);
    if (!strcmp(name, "splice"))     return js_native_fn(name, jsb_splice);
    if (!strcmp(name, "indexOf"))    return js_native_fn(name, jsb_indexOf_a);
    if (!strcmp(name, "includes"))   return js_native_fn(name, jsb_includes_a);
    if (!strcmp(name, "join"))       return js_native_fn(name, jsb_join);
    if (!strcmp(name, "concat"))     return js_native_fn(name, jsb_concat_a);
    if (!strcmp(name, "reverse"))    return js_native_fn(name, jsb_reverse);
    if (!strcmp(name, "sort"))       return js_native_fn(name, jsb_sort);
    if (!strcmp(name, "forEach"))    return js_native_fn(name, jsb_forEach);
    if (!strcmp(name, "map"))        return js_native_fn(name, jsb_map);
    if (!strcmp(name, "filter"))     return js_native_fn(name, jsb_filter);
    if (!strcmp(name, "find"))       return js_native_fn(name, jsb_find);
    if (!strcmp(name, "findIndex"))  return js_native_fn(name, jsb_findIndex);
    if (!strcmp(name, "some"))       return js_native_fn(name, jsb_some);
    if (!strcmp(name, "every"))      return js_native_fn(name, jsb_every);
    if (!strcmp(name, "reduce"))     return js_native_fn(name, jsb_reduce);
    if (!strcmp(name, "fill"))       return js_native_fn(name, jsb_fill);
    if (!strcmp(name, "flat"))       return js_native_fn(name, jsb_flat);
    if (!strcmp(name, "at"))         return js_native_fn(name, jsb_at);
    if (!strcmp(name, "join"))       return js_native_fn(name, jsb_join);
    return js_undef();
}

/* ==========================================================================
 * objects and functions
 * ========================================================================== */

static js_val jsb_hasOwn(js_val s, js_val *a, int n) {
    if (s.t != JS_OBJ || !s.u.o) return js_bool(0);
    return js_bool(js_find(s.u.o, js_cstr(js_arg(a, n, 0))) != 0);
}
static js_val js_object_prop(js_val base, const char *name) {
    (void)base;
    if (!strcmp(name, "hasOwnProperty")) return js_native_fn(name, jsb_hasOwn);
    if (!strcmp(name, "toString"))       return js_native_fn(name, jsb_tostring);
    return js_undef();
}

static js_val jsb_call(js_val s, js_val *a, int n) {
    return js_call(s, js_arg(a, n, 0), a + (n ? 1 : 0), n ? n - 1 : 0);
}
static js_val jsb_apply(js_val s, js_val *a, int n) {
    js_obj *arr = js_arr(js_arg(a, n, 1));
    return js_call(s, js_arg(a, n, 0), arr ? arr->elems : 0, arr ? arr->nelems : 0);
}
/* bind, kept simple: the bound function remembers `this` and nothing else. */
static js_obj *js_bind_target;
static js_val  js_bind_self;
static js_val jsb_bound(js_val s, js_val *a, int n) {
    (void)s;
    return js_call(js_obj_val(js_bind_target), js_bind_self, a, n);
}
static js_val jsb_bind(js_val s, js_val *a, int n) {
    if (s.t != JS_OBJ) return s;
    js_bind_target = s.u.o;
    js_bind_self = js_arg(a, n, 0);
    return js_native_fn("bound", jsb_bound);
}
static js_val js_function_prop(js_val base, const char *name) {
    if (!strcmp(name, "call"))  return js_native_fn(name, jsb_call);
    if (!strcmp(name, "apply")) return js_native_fn(name, jsb_apply);
    if (!strcmp(name, "bind"))  return js_native_fn(name, jsb_bind);
    if (!strcmp(name, "name"))
        return js_str(base.u.o->cname ? base.u.o->cname :
                      (base.u.o->fn && base.u.o->fn->op ? base.u.o->fn->op : ""));
    if (!strcmp(name, "length"))
        return js_num(base.u.o->fn ? base.u.o->fn->nlist : 0);
    return js_undef();
}

/* ==========================================================================
 * the globals
 * ========================================================================== */

static js_val jsg_log(js_val s, js_val *a, int n) {
    (void)s;
    static char line[2048];
    int len = 0;
    for (int i = 0; i < n; i++) {
        if (i && len < (int)sizeof line - 1) line[len++] = ' ';
        js_val t = js_to_string(a[i]);
        for (int k = 0; k < t.u.s.len && len < (int)sizeof line - 1; k++)
            line[len++] = t.u.s.p[k];
    }
    line[len] = 0;
    if (js_console_sink) js_console_sink(line);
    return js_undef();
}

static js_val jsg_parseInt(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_to_string(js_arg(a, n, 0));
    int radix = n > 1 ? (int)js_to_num(a[1]) : 10;
    if (radix == 0) radix = 10;
    const char *p = v.u.s.p;
    while (*p && js_ws((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (radix == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (n <= 1 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
    double acc = 0;
    int any = 0;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= radix) break;
        acc = acc * radix + d;
        any = 1;
    }
    if (!any) return js_num(0.0 / 0.0);
    return js_num(neg ? -acc : acc);
}
static js_val jsg_parseFloat(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_to_string(js_arg(a, n, 0));
    const char *p = v.u.s.p;
    while (*p && js_ws((unsigned char)*p)) p++;
    char *end;
    double d = strtod(p, &end);
    if (end == p) return js_num(0.0 / 0.0);
    return js_num(d);
}
static js_val jsg_isNaN(js_val s, js_val *a, int n) {
    (void)s;
    double d = js_to_num(js_arg(a, n, 0));
    return js_bool(d != d);
}
static js_val jsg_isFinite(js_val s, js_val *a, int n) {
    (void)s;
    double d = js_to_num(js_arg(a, n, 0));
    return js_bool(d == d && d != 1.0/0.0 && d != -1.0/0.0);
}
static js_val jsg_String(js_val s, js_val *a, int n)  { (void)s; return js_to_string(js_arg(a, n, 0)); }
static js_val jsg_Number(js_val s, js_val *a, int n)  { (void)s; return js_num(js_to_num(js_arg(a, n, 0))); }
static js_val jsg_Boolean(js_val s, js_val *a, int n) { (void)s; return js_bool(js_truthy(js_arg(a, n, 0))); }

/* --- Math ------------------------------------------------------------------ */

#define JSM1(nm, expr) \
    static js_val jsm_##nm(js_val s, js_val *a, int n) { \
        (void)s; double x = js_to_num(js_arg(a, n, 0)); (void)x; return js_num(expr); }

JSM1(abs,   x < 0 ? -x : x)
JSM1(floor, floor(x))
JSM1(ceil,  ceil(x))
JSM1(sqrt,  sqrt(x))
JSM1(sin,   sin(x))
JSM1(cos,   cos(x))
JSM1(tan,   tan(x))
JSM1(atan,  atan(x))
JSM1(asin,  asin(x))
JSM1(acos,  acos(x))
JSM1(log,   log(x))
JSM1(log2,  log(x) / log(2.0))
JSM1(log10, log10(x))
JSM1(exp,   exp(x))
JSM1(sign,  x > 0 ? 1 : x < 0 ? -1 : x)
JSM1(trunc, x < 0 ? ceil(x) : floor(x))
JSM1(cbrt,  x < 0 ? -pow(-x, 1.0/3.0) : pow(x, 1.0/3.0))

static js_val jsm_round(js_val s, js_val *a, int n) {
    (void)s;
    /* JavaScript rounds halves upward, which is not what round() does for
     * negatives: Math.round(-0.5) is 0, not -1. */
    return js_num(floor(js_to_num(js_arg(a, n, 0)) + 0.5));
}
static js_val jsm_pow(js_val s, js_val *a, int n) {
    (void)s;
    return js_num(pow(js_to_num(js_arg(a, n, 0)), js_to_num(js_arg(a, n, 1))));
}
static js_val jsm_atan2(js_val s, js_val *a, int n) {
    (void)s;
    return js_num(atan2(js_to_num(js_arg(a, n, 0)), js_to_num(js_arg(a, n, 1))));
}
static js_val jsm_min(js_val s, js_val *a, int n) {
    (void)s;
    if (!n) return js_num(1.0 / 0.0);
    double m = js_to_num(a[0]);
    for (int i = 1; i < n; i++) { double v = js_to_num(a[i]); if (v < m) m = v; }
    return js_num(m);
}
static js_val jsm_max(js_val s, js_val *a, int n) {
    (void)s;
    if (!n) return js_num(-1.0 / 0.0);
    double m = js_to_num(a[0]);
    for (int i = 1; i < n; i++) { double v = js_to_num(a[i]); if (v > m) m = v; }
    return js_num(m);
}
/* A small linear congruential generator. Nothing here is security-sensitive,
 * and a page that wants randomness wants it to look random, not to be. */
static unsigned long js_rand_state = 0x2545F4914F6CDD1DUL;
static js_val jsm_random(js_val s, js_val *a, int n) {
    (void)s; (void)a; (void)n;
    js_rand_state = js_rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return js_num((double)((js_rand_state >> 11) & 0x1FFFFFFFFFFFFFUL) /
                  (double)0x20000000000000UL);
}

/* --- JSON ------------------------------------------------------------------- */

static void js_json_write(js_val v, char *buf, int *len, int max) {
    switch (v.t) {
    case JS_UNDEF: case JS_NULL:
        for (const char *p = "null"; *p && *len < max - 1; p++) buf[(*len)++] = *p;
        return;
    case JS_BOOL:
        for (const char *p = v.u.b ? "true" : "false"; *p && *len < max - 1; p++)
            buf[(*len)++] = *p;
        return;
    case JS_NUM: {
        char t[64];
        js_fmt_num(v.u.n, t, sizeof t);
        for (const char *p = t; *p && *len < max - 1; p++) buf[(*len)++] = *p;
        return;
    }
    case JS_STR: {
        if (*len < max - 1) buf[(*len)++] = '"';
        for (int i = 0; i < v.u.s.len && *len < max - 8; i++) {
            char c = v.u.s.p[i];
            if (c == '"' || c == '\\') { buf[(*len)++] = '\\'; buf[(*len)++] = c; }
            else if (c == '\n') { buf[(*len)++] = '\\'; buf[(*len)++] = 'n'; }
            else if (c == '\t') { buf[(*len)++] = '\\'; buf[(*len)++] = 't'; }
            else if ((unsigned char)c < 0x20) {
                *len += snprintf(buf + *len, (size_t)(max - *len), "\\u%04x", c);
            } else buf[(*len)++] = c;
        }
        if (*len < max - 1) buf[(*len)++] = '"';
        return;
    }
    default: {
        js_obj *o = v.u.o;
        if (!o) return;
        if (o->kind == JSO_ARRAY) {
            if (*len < max - 1) buf[(*len)++] = '[';
            for (int i = 0; i < o->nelems; i++) {
                if (i && *len < max - 1) buf[(*len)++] = ',';
                js_json_write(o->elems[i], buf, len, max);
            }
            if (*len < max - 1) buf[(*len)++] = ']';
            return;
        }
        if (o->kind == JSO_FUNC || o->kind == JSO_NATIVE) {
            for (const char *p = "null"; *p && *len < max - 1; p++) buf[(*len)++] = *p;
            return;
        }
        if (*len < max - 1) buf[(*len)++] = '{';
        for (int i = 0; i < o->nprops; i++) {
            if (i && *len < max - 1) buf[(*len)++] = ',';
            js_json_write(js_str(o->props[i].name), buf, len, max);
            if (*len < max - 1) buf[(*len)++] = ':';
            js_json_write(o->props[i].v, buf, len, max);
        }
        if (*len < max - 1) buf[(*len)++] = '}';
        return;
    }
    }
}

static js_val jsj_stringify(js_val s, js_val *a, int n) {
    (void)s;
    static char buf[65536];
    int len = 0;
    js_json_write(js_arg(a, n, 0), buf, &len, (int)sizeof buf);
    buf[len] = 0;
    return js_sv(buf, len);
}

static const char *js_json_parse(const char *p, const char *end, js_val *out);

static const char *js_json_ws(const char *p, const char *end) {
    while (p < end && js_ws((unsigned char)*p)) p++;
    return p;
}

static const char *js_json_parse(const char *p, const char *end, js_val *out) {
    p = js_json_ws(p, end);
    if (p >= end) { *out = js_undef(); return p; }

    if (*p == '{') {
        js_obj *o = js_new(JSO_PLAIN);
        p++;
        p = js_json_ws(p, end);
        while (p < end && *p != '}') {
            js_val k;
            p = js_json_parse(p, end, &k);
            p = js_json_ws(p, end);
            if (p < end && *p == ':') p++;
            js_val v;
            p = js_json_parse(p, end, &v);
            js_set(o, js_cstr(k), v);
            p = js_json_ws(p, end);
            if (p < end && *p == ',') { p++; p = js_json_ws(p, end); }
        }
        if (p < end) p++;
        *out = js_obj_val(o);
        return p;
    }
    if (*p == '[') {
        js_obj *o = js_new_array();
        p++;
        p = js_json_ws(p, end);
        while (p < end && *p != ']') {
            js_val v;
            p = js_json_parse(p, end, &v);
            js_arr_push(o, v);
            p = js_json_ws(p, end);
            if (p < end && *p == ',') { p++; p = js_json_ws(p, end); }
        }
        if (p < end) p++;
        *out = js_obj_val(o);
        return p;
    }
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        static char buf[8192];
        int n = 0;
        while (p < end && *p != q) {
            char c = *p++;
            if (c == '\\' && p < end) {
                char e = *p++;
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    int cp = 0;
                    for (int k = 0; k < 4 && p < end; k++) {
                        int d = *p++, x;
                        if (d >= '0' && d <= '9') x = d - '0';
                        else if (d >= 'a' && d <= 'f') x = d - 'a' + 10;
                        else if (d >= 'A' && d <= 'F') x = d - 'A' + 10;
                        else break;
                        cp = cp * 16 + x;
                    }
                    if (cp < 0x80) { if (n < (int)sizeof buf) buf[n++] = (char)cp; }
                    else if (cp < 0x800) {
                        if (n + 2 < (int)sizeof buf) {
                            buf[n++] = (char)(0xC0 | (cp >> 6));
                            buf[n++] = (char)(0x80 | (cp & 63));
                        }
                    } else if (n + 3 < (int)sizeof buf) {
                        buf[n++] = (char)(0xE0 | (cp >> 12));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 63));
                        buf[n++] = (char)(0x80 | (cp & 63));
                    }
                    continue;
                }
                default: c = e;
                }
            }
            if (n < (int)sizeof buf) buf[n++] = c;
        }
        if (p < end) p++;
        *out = js_sv(buf, n);
        return p;
    }
    if (!strncmp(p, "true", 4))  { *out = js_bool(1); return p + 4; }
    if (!strncmp(p, "false", 5)) { *out = js_bool(0); return p + 5; }
    if (!strncmp(p, "null", 4))  { *out = js_null();  return p + 4; }

    char *e2;
    double d = strtod(p, &e2);
    if (e2 == p) { *out = js_undef(); return p + 1; }
    *out = js_num(d);
    return e2;
}

static js_val jsj_parse(js_val s, js_val *a, int n) {
    (void)s;
    js_val src = js_to_string(js_arg(a, n, 0));
    js_val out;
    js_json_parse(src.u.s.p, src.u.s.p + src.u.s.len, &out);
    return out;
}

/* --- Object.keys and friends -------------------------------------------------- */

static js_val jso_keys(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_arg(a, n, 0);
    js_obj *out = js_new_array();
    if (v.t == JS_OBJ && v.u.o) {
        if (v.u.o->kind == JSO_ARRAY)
            for (int i = 0; i < v.u.o->nelems; i++) {
                char b[24]; snprintf(b, sizeof b, "%d", i);
                js_arr_push(out, js_str(b));
            }
        else
            for (int i = 0; i < v.u.o->nprops; i++)
                js_arr_push(out, js_str(v.u.o->props[i].name));
    }
    return js_obj_val(out);
}
static js_val jso_values(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_arg(a, n, 0);
    js_obj *out = js_new_array();
    if (v.t == JS_OBJ && v.u.o) {
        if (v.u.o->kind == JSO_ARRAY)
            for (int i = 0; i < v.u.o->nelems; i++) js_arr_push(out, v.u.o->elems[i]);
        else
            for (int i = 0; i < v.u.o->nprops; i++) js_arr_push(out, v.u.o->props[i].v);
    }
    return js_obj_val(out);
}
static js_val jso_entries(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_arg(a, n, 0);
    js_obj *out = js_new_array();
    if (v.t == JS_OBJ && v.u.o && v.u.o->kind != JSO_ARRAY)
        for (int i = 0; i < v.u.o->nprops; i++) {
            js_obj *pair = js_new_array();
            js_arr_push(pair, js_str(v.u.o->props[i].name));
            js_arr_push(pair, v.u.o->props[i].v);
            js_arr_push(out, js_obj_val(pair));
        }
    return js_obj_val(out);
}
static js_val jso_assign(js_val s, js_val *a, int n) {
    (void)s;
    js_val t = js_arg(a, n, 0);
    if (t.t != JS_OBJ || !t.u.o) return t;
    for (int i = 1; i < n; i++)
        if (a[i].t == JS_OBJ && a[i].u.o)
            for (int k = 0; k < a[i].u.o->nprops; k++)
                js_set(t.u.o, a[i].u.o->props[k].name, a[i].u.o->props[k].v);
    return t;
}

static js_val jsa_isArray(js_val s, js_val *a, int n) {
    (void)s;
    return js_bool(js_arr(js_arg(a, n, 0)) != 0);
}
static js_val jsa_from(js_val s, js_val *a, int n) {
    (void)s;
    js_val v = js_arg(a, n, 0);
    js_obj *out = js_new_array();
    js_obj *src = js_arr(v);
    if (src) for (int i = 0; i < src->nelems; i++) js_arr_push(out, src->elems[i]);
    else if (v.t == JS_STR)
        for (int i = 0; i < v.u.s.len; i++) js_arr_push(out, js_sv(v.u.s.p + i, 1));
    else if (v.t == JS_OBJ && v.u.o) {
        js_val *ln = js_find(v.u.o, "length");
        int cnt = ln ? (int)js_to_num(*ln) : 0;
        for (int i = 0; i < cnt; i++) {
            char b[24]; snprintf(b, sizeof b, "%d", i);
            js_val *e = js_find(v.u.o, b);
            js_arr_push(out, e ? *e : js_undef());
        }
    }
    return js_obj_val(out);
}

/* --- Date, barely ------------------------------------------------------------- */

static js_val jsd_now(js_val s, js_val *a, int n) {
    (void)s; (void)a; (void)n;
    return js_num(js_clock_ms ? js_clock_ms() : 0);
}

/* --- what `new X(...)` does for the built-in names ------------------------- */

static js_val js_construct_builtin(js_node *callee, js_val fn, js_val *args, int nargs) {
    (void)fn;
    if (!callee || callee->type != N_IDENT || !callee->op) return js_undef();
    const char *nm = callee->op;

    if (!strcmp(nm, "Array")) {
        js_obj *o = js_new_array();
        if (nargs == 1 && args[0].t == JS_NUM) {
            int k = (int)args[0].u.n;
            for (int i = 0; i < k && i < 100000; i++) js_arr_push(o, js_undef());
        } else for (int i = 0; i < nargs; i++) js_arr_push(o, args[i]);
        return js_obj_val(o);
    }
    if (!strcmp(nm, "Object")) return js_obj_val(js_new(JSO_PLAIN));
    if (!strcmp(nm, "Date")) {
        js_obj *o = js_new(JSO_PLAIN);
        js_def(o, "getTime", jsd_now);
        js_def(o, "valueOf", jsd_now);
        return js_obj_val(o);
    }
    if (!strcmp(nm, "Error") || !strcmp(nm, "TypeError") ||
        !strcmp(nm, "RangeError")) {
        js_obj *o = js_new(JSO_PLAIN);
        js_set(o, "message", nargs ? js_to_string(args[0]) : js_str(""));
        js_set(o, "name", js_str(nm));
        return js_obj_val(o);
    }
    return js_undef();
}

/* --- assembling the global object -------------------------------------------- */

static js_obj *js_globals;

static void js_setup_globals(void) {
    js_globals = js_global_scope->vars;

    js_obj *console = js_new(JSO_PLAIN);
    js_def(console, "log", jsg_log);
    js_def(console, "info", jsg_log);
    js_def(console, "warn", jsg_log);
    js_def(console, "error", jsg_log);
    js_def(console, "debug", jsg_log);
    js_set(js_globals, "console", js_obj_val(console));

    js_obj *math = js_new(JSO_PLAIN);
    js_def(math, "abs", jsm_abs);     js_def(math, "floor", jsm_floor);
    js_def(math, "ceil", jsm_ceil);   js_def(math, "sqrt", jsm_sqrt);
    js_def(math, "sin", jsm_sin);     js_def(math, "cos", jsm_cos);
    js_def(math, "tan", jsm_tan);     js_def(math, "atan", jsm_atan);
    js_def(math, "asin", jsm_asin);   js_def(math, "acos", jsm_acos);
    js_def(math, "log", jsm_log);     js_def(math, "log2", jsm_log2);
    js_def(math, "log10", jsm_log10); js_def(math, "exp", jsm_exp);
    js_def(math, "sign", jsm_sign);   js_def(math, "trunc", jsm_trunc);
    js_def(math, "cbrt", jsm_cbrt);   js_def(math, "round", jsm_round);
    js_def(math, "pow", jsm_pow);     js_def(math, "atan2", jsm_atan2);
    js_def(math, "min", jsm_min);     js_def(math, "max", jsm_max);
    js_def(math, "random", jsm_random);
    js_set(math, "PI", js_num(3.14159265358979323846));
    js_set(math, "E",  js_num(2.71828182845904523536));
    js_set(math, "LN2", js_num(0.69314718055994530942));
    js_set(math, "SQRT2", js_num(1.41421356237309504880));
    js_set(js_globals, "Math", js_obj_val(math));

    js_obj *json = js_new(JSO_PLAIN);
    js_def(json, "stringify", jsj_stringify);
    js_def(json, "parse", jsj_parse);
    js_set(js_globals, "JSON", js_obj_val(json));

    js_obj *objc = js_new(JSO_NATIVE);
    objc->native = jsg_Boolean;             /* Object(x) is rarely meant */
    js_def(objc, "keys", jso_keys);
    js_def(objc, "values", jso_values);
    js_def(objc, "entries", jso_entries);
    js_def(objc, "assign", jso_assign);
    js_set(js_globals, "Object", js_obj_val(objc));

    js_obj *arrc = js_new(JSO_NATIVE);
    arrc->native = jsa_from;
    js_def(arrc, "isArray", jsa_isArray);
    js_def(arrc, "from", jsa_from);
    js_set(js_globals, "Array", js_obj_val(arrc));

    js_obj *datec = js_new(JSO_PLAIN);
    js_def(datec, "now", jsd_now);
    js_set(js_globals, "Date", js_obj_val(datec));

    js_obj *numc = js_new(JSO_NATIVE);
    numc->native = jsg_Number;
    js_def(numc, "parseInt", jsg_parseInt);
    js_def(numc, "parseFloat", jsg_parseFloat);
    js_def(numc, "isNaN", jsg_isNaN);
    js_def(numc, "isFinite", jsg_isFinite);
    js_set(numc, "MAX_SAFE_INTEGER", js_num(9007199254740991.0));
    js_set(numc, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    js_set(js_globals, "Number", js_obj_val(numc));

    js_set(js_globals, "String", js_native_fn("String", jsg_String));
    js_set(js_globals, "Boolean", js_native_fn("Boolean", jsg_Boolean));
    js_set(js_globals, "parseInt", js_native_fn("parseInt", jsg_parseInt));
    js_set(js_globals, "parseFloat", js_native_fn("parseFloat", jsg_parseFloat));
    js_set(js_globals, "isNaN", js_native_fn("isNaN", jsg_isNaN));
    js_set(js_globals, "isFinite", js_native_fn("isFinite", jsg_isFinite));
    js_set(js_globals, "NaN", js_num(0.0 / 0.0));
    js_set(js_globals, "Infinity", js_num(1.0 / 0.0));
}

#endif /* JSBUILTIN_H */
