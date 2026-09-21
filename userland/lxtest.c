/* Lexbor on xyuOS: does it actually work, or does it merely link.
 *
 * Three things, chosen because each of them is somewhere this system has
 * been wrong before:
 *
 *   The HTML the tree builder is given is deliberately awful -- unquoted
 *   attributes with spaces in them, a <script> full of things that look like
 *   tags, mis-nested formatting, a table with the rows not wrapped. Every one
 *   of those is a bug that was found and fixed by hand in userland/dom.h.
 *   The specification's tree construction algorithm handles them by
 *   construction, and this prints what it made of them.
 *
 *   Then a CSS selector, run against that tree.
 *
 *   Then KOI8-R to UTF-8 -- the encoding libparserutils does not carry, which
 *   is why NetSurf's iconv refuses it here.
 */

#include <stdio.h>
#include <string.h>

#include "lexbor/html/html.h"
#include "lexbor/css/css.h"
#include "lexbor/selectors/selectors.h"
#include "lexbor/encoding/encoding.h"

/* The parser is handed this. It is not valid in any sense; the point is that
 * the answer is still well defined. */
static const lxb_char_t MESSY[] =
    "<html><head>"
    "<script>var s = \"<div class='fake'>not a tag</div>\"; if (a<b) {}</script>"
    "<title>a messy page</title>"
    "</head><body>"
    "<p>one<p>two"                       /* paragraphs never closed        */
    "<b><i>bold and italic</b> only italic</i>"   /* mis-nested            */
    "<table><tr><td class=cell one>a cell</table>" /* no tbody, odd attrs  */
    "<div id=main><span>text</div>"      /* span left open across a div    */
    "</body></html>";

static lxb_status_t put(const lxb_char_t *data, size_t len, void *ctx) {
    (void)ctx;
    fwrite(data, 1, len, stdout);
    return LXB_STATUS_OK;
}

static unsigned found;

static lxb_status_t hit(lxb_dom_node_t *node,
                        lxb_css_selector_specificity_t spec, void *ctx) {
    (void)spec; (void)ctx;
    printf("   %u) ", ++found);
    (void)lxb_html_serialize_cb(node, put, NULL);
    printf("\n");
    return LXB_STATUS_OK;
}

static int parse_and_query(void) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (doc == NULL) { printf("could not make a document\n"); return 1; }

    if (lxb_html_document_parse(doc, MESSY, sizeof MESSY - 1)
            != LXB_STATUS_OK) {
        printf("the parser refused it\n");
        return 1;
    }

    printf("--- what the tree builder made of it ---\n");
    (void)lxb_html_serialize_pretty_tree_cb(lxb_dom_interface_node(doc),
                                            LXB_HTML_SERIALIZE_OPT_UNDEF,
                                            0, put, NULL);

    /* Now ask it a question in CSS. */
    static const lxb_char_t SEL[] = "td, div#main span, p";

    lxb_css_parser_t *parser = lxb_css_parser_create();
    if (lxb_css_parser_init(parser, NULL) != LXB_STATUS_OK) return 1;

    lxb_selectors_t *sel = lxb_selectors_create();
    if (lxb_selectors_init(sel) != LXB_STATUS_OK) return 1;

    lxb_css_selector_list_t *list =
        lxb_css_selectors_parse(parser, SEL, sizeof SEL - 1);
    if (parser->status != LXB_STATUS_OK) {
        printf("the selector did not parse\n");
        return 1;
    }

    printf("\n--- everything matching  %s  ---\n", (const char *)SEL);
    found = 0;
    (void)lxb_selectors_find(sel, lxb_dom_interface_node(doc), list,
                             hit, NULL);
    if (found == 0) printf("   (nothing)\n");

    lxb_selectors_destroy(sel, true);
    lxb_css_parser_destroy(parser, true);
    lxb_css_selector_list_destroy_memory(list);
    lxb_html_document_destroy(doc);
    return 0;
}

/* One line of Russian, in KOI8-R, byte by byte. Writing it as bytes rather
 * than as a string in this file is deliberate: what is being tested is the
 * decoder, not this compiler's idea of what the source file is encoded in. */
static const unsigned char KOI8[] = {
    0xF0, 0xD2, 0xC9, 0xD7, 0xC5, 0xD4, 0x2C, 0x20,             /* Привет,  */
    0xCD, 0xC9, 0xD2, 0x21, 0x20,                               /* мир!     */
    0xFB, 0xC5, 0xD3, 0xD4, 0xD8, 0x20, 0xC5, 0xDD, 0xC5, 0x20, /* Шесть ещё */
    0xDA, 0xD9, 0xC2, 0xD1, 0xCB                                /* зыбкий-ish */
};

static int convert(const char *from_name, const char *to_name,
                   const unsigned char *in, size_t inlen) {
    const lxb_encoding_data_t *from =
        lxb_encoding_data_by_pre_name((const lxb_char_t *)from_name,
                                      strlen(from_name));
    const lxb_encoding_data_t *to =
        lxb_encoding_data_by_pre_name((const lxb_char_t *)to_name,
                                      strlen(to_name));
    if (from == NULL || to == NULL) {
        printf("   %s -> %s: one of those is not an encoding it knows\n",
               from_name, to_name);
        return 1;
    }

    lxb_encoding_decode_t dec;
    lxb_encoding_encode_t enc;
    if (lxb_encoding_decode_init_single(&dec, from) != LXB_STATUS_OK) return 1;
    if (lxb_encoding_encode_init_single(&enc, to) != LXB_STATUS_OK) return 1;

    printf("   %s -> %s: ", from_name, to_name);

    const lxb_char_t *p = (const lxb_char_t *)in;
    const lxb_char_t *end = p + inlen;

    while (p < end) {
        lxb_codepoint_t cp = from->decode_single(&dec, &p, end);
        if (cp > LXB_ENCODING_DECODE_MAX_CODEPOINT) {
            if (cp == LXB_ENCODING_DECODE_CONTINUE) break;
            cp = LXB_ENCODING_REPLACEMENT_CODEPOINT;
        }

        lxb_char_t outbuf[16];
        lxb_char_t *out = outbuf;
        int8_t len = to->encode_single(&enc, &out, outbuf + sizeof outbuf, cp);
        if (len < LXB_ENCODING_ENCODE_OK) { printf("?"); continue; }
        fwrite(outbuf, 1, (size_t)len, stdout);
    }
    printf("\n");
    return 0;
}

/* Two halves, because one screen does not hold both. `lxtest tree` shows
 * what the parser made of the mess; `lxtest enc` shows the encodings. */
int main(int argc, char **argv) {
    const char *what = (argc > 1) ? argv[1] : "tree";

    printf("Lexbor %s\n\n", LEXBOR_VERSION_STRING);

    if (what[0] == 't') return parse_and_query();

    printf("--- encodings, including the one libparserutils lacks ---\n");
    convert("koi8-r", "utf-8", KOI8, sizeof KOI8);

    /* And the same text round-tripped through two more, to show the table is
     * real rather than one special case. */
    unsigned char w1251[64];
    {
        const lxb_encoding_data_t *from =
            lxb_encoding_data_by_pre_name((const lxb_char_t *)"koi8-r", 6);
        const lxb_encoding_data_t *to =
            lxb_encoding_data_by_pre_name((const lxb_char_t *)"windows-1251", 12);
        lxb_encoding_decode_t dec;
        lxb_encoding_encode_t enc;
        lxb_encoding_decode_init_single(&dec, from);
        lxb_encoding_encode_init_single(&enc, to);

        const lxb_char_t *p = KOI8, *end = KOI8 + sizeof KOI8;
        size_t n = 0;
        while (p < end && n < sizeof w1251) {
            lxb_codepoint_t cp = from->decode_single(&dec, &p, end);
            if (cp > LXB_ENCODING_DECODE_MAX_CODEPOINT) break;
            lxb_char_t *out = w1251 + n;
            int8_t len = to->encode_single(&enc, &out, w1251 + sizeof w1251, cp);
            if (len < LXB_ENCODING_ENCODE_OK) break;
            n += (size_t)len;
        }
        printf("   koi8-r -> windows-1251: %d bytes\n", (int)n);
        convert("windows-1251", "utf-8", w1251, n);
    }

    printf("\nall of it ran on xyuOS.\n");
    return 0;
}
