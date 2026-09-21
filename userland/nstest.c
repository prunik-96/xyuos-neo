/* Does NetSurf's engine run on xyuOS?
 *
 * The libraries compiling proves nothing about whether they work: a parser
 * that links but cannot allocate, or an interner whose hash lands wrong, is a
 * parser that says everything is fine and returns an empty tree. So this
 * feeds each of them something small with a known answer and prints what came
 * back -- run on the OS itself, not on the host.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libwapcaplet/libwapcaplet.h>
#include <parserutils/parserutils.h>
#include <hubbub/parser.h>
#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>
#include <libcss/libcss.h>

static int failures;

static void ok(const char *what, int good, const char *saw) {
    printf("%-42s %s%s%s\n", what, good ? "yes" : "NO",
           saw && saw[0] ? "  -- " : "", saw ? saw : "");
    if (!good) failures++;
}

/* --- libwapcaplet: the string table everything else is built on ---------- */
static void try_wapcaplet(void) {
    lwc_string *a = NULL, *b = NULL, *c = NULL;
    bool same = false;
    char saw[128];

    lwc_intern_string("paragraph", 9, &a);
    lwc_intern_string("paragraph", 9, &b);
    lwc_intern_string("division", 8, &c);

    ok("interning a string", a != NULL, a ? lwc_string_data(a) : "");
    /* The whole point of interning: the same word twice is one object. */
    ok("the same word twice is one object", a == b, "");
    lwc_string_isequal(a, c, &same);
    ok("and a different word is not", !same, "");

    snprintf(saw, sizeof saw, "%u", (unsigned)(a ? lwc_string_length(a) : 0));
    ok("its length survives the trip", a && lwc_string_length(a) == 9, saw);

    if (a) lwc_string_unref(a);
    if (b) lwc_string_unref(b);
    if (c) lwc_string_unref(c);
}

/* --- libdom + libhubbub: real HTML into a real tree ---------------------- */
static void try_html(void) {
    static const char *HTML =
        "<!doctype html><html><head><title>A page</title></head>"
        "<body><h1 id=\"top\">Heading</h1>"
        "<p class=\"lead\">First paragraph.</p>"
        "<p>Second <b>paragraph</b>.</p>"
        "<ul><li>one</li><li>two</li><li>three</li></ul>"
        "</body></html>";

    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = NULL;
    dom_document *doc = NULL;
    dom_hubbub_error err;
    char saw[160];

    params.enc = NULL;
    params.fix_enc = true;
    params.enable_script = false;
    params.msg = NULL;
    params.script = NULL;
    params.ctx = NULL;
    params.daf = NULL;

    err = dom_hubbub_parser_create(&params, &parser, &doc);
    ok("making an HTML parser", err == DOM_HUBBUB_OK, "");
    if (err != DOM_HUBBUB_OK) return;

    err = dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)HTML,
                                        strlen(HTML));
    ok("feeding it a page", err == DOM_HUBBUB_OK, "");
    err = dom_hubbub_parser_completed(parser);
    ok("and finishing it", err == DOM_HUBBUB_OK, "");

    /* The title, read back out of the tree -- by finding the element and
     * asking it for its text, which is a harder thing to get right than a
     * convenience call and tells us more when it works. */
    {
        dom_string *tname = NULL, *text = NULL;
        dom_nodelist *tl = NULL;
        dom_node *tnode = NULL;
        uint32_t tn = 0;
        dom_string_create((const uint8_t *)"title", 5, &tname);
        if (dom_document_get_elements_by_tag_name(doc, tname, &tl) == DOM_NO_ERR
            && tl) {
            dom_nodelist_get_length(tl, &tn);
            if (tn > 0) dom_nodelist_item(tl, 0, &tnode);
            dom_nodelist_unref(tl);
        }
        dom_string_unref(tname);
        if (tnode && dom_node_get_text_content(tnode, &text) == DOM_NO_ERR &&
            text) {
            snprintf(saw, sizeof saw, "%s", dom_string_data(text));
            ok("the title is in the tree",
               strcmp(dom_string_data(text), "A page") == 0, saw);
            dom_string_unref(text);
        } else {
            ok("the title is in the tree", 0, "not found");
        }
        if (tnode) dom_node_unref(tnode);
    }

    /* Every <p>, counted -- which means the tree really has a shape. */
    dom_string *tag = NULL;
    dom_nodelist *list = NULL;
    uint32_t n = 0;
    dom_string_create((const uint8_t *)"p", 1, &tag);
    if (dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR &&
        list) {
        dom_nodelist_get_length(list, &n);
        dom_nodelist_unref(list);
    }
    dom_string_unref(tag);
    snprintf(saw, sizeof saw, "%u", (unsigned)n);
    ok("two paragraphs found", n == 2, saw);

    dom_string_create((const uint8_t *)"li", 2, &tag);
    n = 0;
    if (dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR &&
        list) {
        dom_nodelist_get_length(list, &n);
        dom_nodelist_unref(list);
    }
    dom_string_unref(tag);
    snprintf(saw, sizeof saw, "%u", (unsigned)n);
    ok("three list items found", n == 3, saw);

    /* And an attribute, because a tree without attributes is half a tree. */
    dom_string *idname = NULL, *idval = NULL;
    dom_element *h1 = NULL;
    dom_string_create((const uint8_t *)"h1", 2, &tag);
    if (dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR &&
        list) {
        dom_nodelist_item(list, 0, (dom_node **)&h1);
        dom_nodelist_unref(list);
    }
    dom_string_unref(tag);
    dom_string_create((const uint8_t *)"id", 2, &idname);
    if (h1 && dom_element_get_attribute(h1, idname, &idval) == DOM_NO_ERR &&
        idval) {
        snprintf(saw, sizeof saw, "%s", dom_string_data(idval));
        ok("the heading knows its id", strcmp(dom_string_data(idval), "top") == 0, saw);
        dom_string_unref(idval);
    } else {
        ok("the heading knows its id", 0, "not found");
    }
    dom_string_unref(idname);
    if (h1) dom_node_unref((dom_node *)h1);

    dom_node_unref(doc);
    dom_hubbub_parser_destroy(parser);
}

/* --- libcss: a stylesheet, parsed ---------------------------------------- */
static css_error resolve_url(void *pw, const char *base,
                             lwc_string *rel, lwc_string **abs) {
    (void)pw; (void)base;
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

static void try_css(void) {
    static const char *SHEET =
        "h1 { color: #ff0000; font-weight: bold }"
        "p.lead { margin-top: 10px; color: rgb(0,128,255) }"
        "@media (min-width: 600px) { p { display: block } }";

    css_stylesheet_params params;
    css_stylesheet *sheet = NULL;
    css_error err;
    char saw[128];

    memset(&params, 0, sizeof params);
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_DEFAULT;
    params.charset = "UTF-8";
    params.url = "http://xyuos/test.css";
    params.title = NULL;
    params.resolve = resolve_url;

    err = css_stylesheet_create(&params, &sheet);
    ok("making a stylesheet", err == CSS_OK, "");
    if (err != CSS_OK) return;

    err = css_stylesheet_append_data(sheet, (const uint8_t *)SHEET,
                                     strlen(SHEET));
    /* NEEDDATA is what it says when it wants more; both mean it is reading. */
    ok("feeding it rules", err == CSS_OK || err == CSS_NEEDDATA, "");

    err = css_stylesheet_data_done(sheet);
    ok("and finishing it", err == CSS_OK, "");

    size_t size = 0;
    css_stylesheet_size(sheet, &size);
    snprintf(saw, sizeof saw, "%u bytes", (unsigned)size);
    ok("the rules took up room", size > 0, saw);

    css_stylesheet_destroy(sheet);
}

int main(void) {
    printf("NetSurf's engine, running on xyuOS\n\n");

    printf("libwapcaplet\n");
    try_wapcaplet();

    printf("\nlibhubbub and libdom\n");
    try_html();

    printf("\nlibcss\n");
    try_css();

    printf("\n%s\n", failures ? "SOMETHING IS WRONG" : "all of it works");
    return failures ? 1 : 0;
}
