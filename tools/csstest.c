/* Ask css.h what it computes, on the host, in a second.
 *
 * A stylesheet question answered by looking at a screenshot of a virtual
 * machine is a question answered slowly and badly. These are the cases that
 * matter: which selectors match, which of two rules wins, and what the
 * cascade does with a property nobody set on the element itself.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* css.h asks the window palette whether a colour would be legible; on the
 * host there is no window manager, so supply one. */
typedef struct { unsigned win, text; } fake_pal;
static fake_pal gui_pal = { 0x111418, 0xD8DEE9 };
#define GC_WIN  (gui_pal.win)
#define GC_TEXT (gui_pal.text)

#include "../userland/css.h"

static int fails, checks;

static void ck(int ok, const char *what) {
    checks++;
    if (!ok) { fails++; printf("  FAIL  %s\n", what); }
}

/* Build a chain from a description like "html > body > div.card > p#lead".
 * Returns the depth. */
#define MAXD 8
static css_elem chain[MAXD];
static char chbuf[MAXD][4][80];

static int build(const char *path) {
    int n = 0;
    const char *p = path;
    while (*p && n < MAXD) {
        while (*p == ' ' || *p == '>') p++;
        if (!*p) break;
        char tok[160];
        int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof tok - 1) tok[k++] = *p++;
        tok[k] = 0;

        char *tag = chbuf[n][0], *id = chbuf[n][1], *cls = chbuf[n][2];
        char *att = chbuf[n][3];
        tag[0] = id[0] = cls[0] = att[0] = 0;

        /* tag[#id][.class...][@attr=value] */
        int i = 0, t = 0;
        while (tok[i] && tok[i] != '#' && tok[i] != '.' && tok[i] != '@') tag[t++] = tok[i++];
        tag[t] = 0;
        while (tok[i]) {
            if (tok[i] == '#') {
                i++; t = 0;
                while (tok[i] && tok[i] != '.' && tok[i] != '@') id[t++] = tok[i++];
                id[t] = 0;
            } else if (tok[i] == '.') {
                i++;
                int c = (int)strlen(cls);
                if (c) cls[c++] = ' ';
                while (tok[i] && tok[i] != '.' && tok[i] != '#' && tok[i] != '@')
                    cls[c++] = tok[i++];
                cls[c] = 0;
            } else if (tok[i] == '@') {
                i++; t = 0;
                while (tok[i]) att[t++] = tok[i++];
                att[t] = 0;
            } else i++;
        }

        memset(&chain[n], 0, sizeof chain[n]);
        chain[n].tag = tag;
        chain[n].id = id[0] ? id : 0;
        chain[n].cls = cls[0] ? cls : 0;
        chain[n].attr = att[0] ? att : 0;
        chain[n].attrlen = (int)strlen(att);
        chain[n].first = 1;
        chain[n].last = 1;
        n++;
    }
    return n;
}

static css_style_t of(const char *path) {
    int n = build(path);
    css_style_t st;
    css_style_init(&st);
    css_match_chain(chain, n, &st);
    return st;
}

/* The root element the variables are resolved against. Tests that care set
 * its attributes; the rest get a plain <html>. */
static css_elem root;
static char root_attr[160];

static void set_root(const char *attrs) {
    memset(&root, 0, sizeof root);
    snprintf(root_attr, sizeof root_attr, "%s", attrs ? attrs : "");
    root.tag = "html";
    root.attr = root_attr;
    root.attrlen = (int)strlen(root_attr);
    root.first = root.last = 1;
}

static void sheet(const char *css) {
    css_reset();
    css_add_source(css, (int)strlen(css));
    css_build(&root, 1);
}

static int coloris(const char *path, unsigned rgb) {
    css_style_t s = of(path);
    return s.has_color && (s.color & 0xFFFFFF) == rgb;
}
static int nocolor(const char *path) { return !of(path).has_color; }

int main(void) {
    css_init();
    set_root("");

    /* --- the selectors the old engine refused ---------------------------- */
    printf("selectors\n");
    sheet("nav a { color: #ff0000 } a { color: #00ff00 }");
    ck(coloris("html body nav a", 0xFF0000), "descendant wins over bare tag");
    ck(coloris("html body main a", 0x00FF00), "bare tag still applies elsewhere");

    sheet("a.btn { color: #112233 } a { color: #445566 }");
    ck(coloris("body a.btn", 0x112233), "compound tag.class");
    ck(coloris("body a", 0x445566), "and does not leak to a plain a");

    sheet("ul > li { color: #010203 }");
    ck(coloris("ul li", 0x010203), "child combinator, direct");
    ck(nocolor("ul div li"), "child combinator, not a grandchild");

    sheet("div p span { color: #0A0B0C }");
    ck(coloris("div section p em span", 0x0A0B0C), "three parts with gaps");
    ck(nocolor("p div span"), "three parts, wrong order");

    sheet(".a .b { color: #223344 }");
    ck(coloris("div.a p.b", 0x223344), "class descendant class");
    ck(nocolor("div.b p.a"), "and not the other way round");

    /* Backtracking: the greedy first match for "a" is wrong here. */
    sheet("x y x z { color: #778899 }");
    ck(coloris("x y x z", 0x778899), "descendant matching backtracks");

    /* --- specificity ------------------------------------------------------ */
    printf("specificity\n");
    sheet("p { color: #111111 } .lead { color: #222222 } #main { color: #333333 }");
    ck(coloris("p.lead#main", 0x333333), "id beats class beats tag");
    sheet("#main { color: #333333 } p.lead { color: #222222 }");
    ck(coloris("p.lead#main", 0x333333), "id still wins when written first");
    sheet("div p { color: #444444 } p { color: #555555 }");
    ck(coloris("div p", 0x444444), "two tags beat one");
    sheet("p { color: #666666 } p { color: #777777 }");
    ck(coloris("p", 0x777777), "equal specificity: the later one");

    /* --- !important ------------------------------------------------------- */
    printf("important\n");
    sheet("p { color: #888888 !important } #x p { color: #999999 }");
    ck(coloris("div#x p", 0x888888), "important beats higher specificity");
    sheet("p { color: #888888 !important } p { color: #999999 !important }");
    ck(coloris("p", 0x999999), "two importants: the later one");

    /* --- attributes ------------------------------------------------------- */
    printf("attributes\n");
    sheet("[hidden] { display: none }");
    ck(of("div@hidden").display == CSS_DISP_NONE, "[attr] present");
    ck(of("div").display != CSS_DISP_NONE, "[attr] absent");

    sheet("input[type=\"submit\"] { color: #AABBCC }");
    ck(coloris("input@type=\"submit\"", 0xAABBCC), "[attr=value]");
    ck(nocolor("input@type=\"text\""), "[attr=value] mismatch");

    sheet("a[href^=\"https\"] { color: #ABCDEF }");
    ck(coloris("a@href=\"https://x\"", 0xABCDEF), "[attr^=prefix]");
    ck(nocolor("a@href=\"http://x\""), "[attr^=prefix] mismatch");

    sheet("a[href$=\".pdf\"] { color: #FEDCBA }");
    ck(coloris("a@href=\"/a/b.pdf\"", 0xFEDCBA), "[attr$=suffix]");

    sheet("a[href*=\"wiki\"] { color: #123456 }");
    ck(coloris("a@href=\"http://en.wiki.org\"", 0x123456), "[attr*=substring]");

    /* --- pseudo-classes --------------------------------------------------- */
    printf("pseudo-classes\n");
    sheet("a:hover { color: #FF00FF } a { color: #00FF00 }");
    ck(coloris("a", 0x00FF00), ":hover never applies");
    sheet("li:first-child { color: #0F0F0F }");
    ck(coloris("ul li", 0x0F0F0F), ":first-child on a first child");
    sheet("p::before { color: #FF0000 } p { color: #00FF00 }");
    ck(coloris("p", 0x00FF00), "::before never applies");
    sheet("p:nth-child(2n) { color: #FF0000 } p { color: #00FF00 }");
    ck(coloris("p", 0x00FF00), "a functional pseudo never applies");
    /* and, crucially, the rest of the list survives it */
    sheet("p:hover, blockquote { color: #5A5A5A }");
    ck(coloris("blockquote", 0x5A5A5A), "a bad selector does not kill its list");

    /* --- media queries ---------------------------------------------------- */
    printf("media queries\n");
    css_viewport_w = 1024;
    sheet("@media screen { p { color: #010101 } }");
    ck(coloris("p", 0x010101), "@media screen applies");
    sheet("@media print { p { color: #020202 } }");
    ck(nocolor("p"), "@media print does not");
    sheet("@media (min-width: 700px) { p { color: #030303 } }");
    ck(coloris("p", 0x030303), "min-width satisfied");
    sheet("@media (min-width: 2000px) { p { color: #040404 } }");
    ck(nocolor("p"), "min-width unsatisfied");
    sheet("@media (max-width: 700px) { p { color: #050505 } }");
    ck(nocolor("p"), "max-width unsatisfied");
    sheet("@media screen and (min-width: 600px) and (max-width: 1200px) { p { color: #060606 } }");
    ck(coloris("p", 0x060606), "a compound query");
    sheet("@media print, screen { p { color: #070707 } }");
    ck(coloris("p", 0x070707), "a query list: one is enough");
    sheet("@media (min-width: 900px) { nav a { color: #080808 } }");
    ck(coloris("body nav a", 0x080808), "selectors inside a query keep working");

    css_viewport_w = 500;
    sheet("@media (max-width: 700px) { p { color: #090909 } }");
    ck(coloris("p", 0x090909), "the same query on a narrow window");
    css_viewport_w = 1024;

    /* --- properties ------------------------------------------------------- */
    printf("properties\n");
    sheet("mark { background-color: #FFFF00 }");
    ck(of("mark").has_bg && (of("mark").bg & 0xFFFFFF) == 0xFFFF00, "background-color");
    sheet("div { background: #123456 url(x.png) no-repeat }");
    ck(of("div").has_bg && (of("div").bg & 0xFFFFFF) == 0x123456, "colour out of the shorthand");
    sheet("div { background: transparent }");
    ck(!of("div").has_bg, "transparent sets nothing");

    sheet("a { text-decoration: none }");
    ck(of("a").decor == CSS_D_NONE, "text-decoration: none");
    sheet("ins { text-decoration: underline }");
    ck(of("ins").decor == CSS_D_UNDER, "text-decoration: underline");

    sheet("th { text-transform: uppercase }");
    ck(of("th").tcase == CSS_T_UPPER, "text-transform: uppercase");

    sheet("aside { opacity: 0 }");
    ck(of("aside").vis == CSS_V_HIDDEN, "opacity: 0 hides");
    sheet("aside { opacity: 0.5 }");
    ck(of("aside").vis != CSS_V_HIDDEN, "a partial opacity does not");

    /* --- in the flow, or out of it ----------------------------------- */
    sheet(".panel { position: absolute }");
    ck(of("div.panel").pos == CSS_POS_ABSOLUTE, "position: absolute");
    sheet(".bar { position: fixed }");
    ck(of("div.bar").pos == CSS_POS_FIXED, "position: fixed");
    sheet(".bar { position: sticky }");
    ck(of("div.bar").pos == CSS_POS_STICKY, "position: sticky");
    sheet(".bar { position: relative }");
    ck(of("div.bar").pos == CSS_POS_RELATIVE, "position: relative");
    sheet("div { position: absolute } .in { position: static }");
    ck(of("div.in").pos == CSS_POS_STATIC, "and put back in the flow");
    sheet("div { color: red }");
    ck(of("div").pos == CSS_POS_AUTO, "nothing said, nothing assumed");
    /* Seventeen property bits were needed and sixteen were all there were. */
    sheet("p { position: absolute; display: flex; gap: 4px; visibility: hidden }");
    ck(of("p").pos == CSS_POS_ABSOLUTE && of("p").display == CSS_DISP_FLEX &&
       of("p").gap == 4 && of("p").vis == CSS_V_HIDDEN,
       "the last property does not push out the first");

    /* --- breakpoints, written the way sheets are written now --------- */
    css_viewport_w = 1150;
    sheet("@media (width >= 1012px) { ul { display: flex } }");
    ck(of("ul").display == CSS_DISP_FLEX, "width >= holds on a wide window");
    sheet("@media (width <= 767px) { ul { display: block } }");
    ck(of("ul").display != CSS_DISP_BLOCK, "and the phone rule does not");
    sheet("@media (width > 1150px) { ul { display: flex } }");
    ck(of("ul").display != CSS_DISP_FLEX, "strictly wider than is strict");
    sheet("@media (width >= 1150px) { ul { display: flex } }");
    ck(of("ul").display == CSS_DISP_FLEX, "at least as wide is not");
    sheet("@media (768px <= width <= 1011px) { ul { display: flex } }");
    ck(of("ul").display != CSS_DISP_FLEX, "a two-ended range we fall outside");
    sheet("@media (768px <= width <= 1400px) { ul { display: flex } }");
    ck(of("ul").display == CSS_DISP_FLEX, "and one we fall inside");
    sheet("@media (1012px <= width) { ul { display: flex } }");
    ck(of("ul").display == CSS_DISP_FLEX, "the value written first");

    css_viewport_w = 700;
    sheet("@media (width >= 1012px) { ul { display: flex } }");
    ck(of("ul").display != CSS_DISP_FLEX, "the desktop rule off a narrow window");
    sheet("@media (width <= 767px) { ul { display: block } }");
    ck(of("ul").display == CSS_DISP_BLOCK, "and the phone rule on");
    sheet("@media (min-width: 1012px) { ul { display: flex } }");
    ck(of("ul").display != CSS_DISP_FLEX, "the old spelling still works");
    sheet("@media (hover: hover) { ul { display: flex } }");
    ck(of("ul").display == CSS_DISP_FLEX, "what we cannot answer still applies");
    css_viewport_w = 1150;

    /* --- what stands in a row ---------------------------------------- */
    sheet("nav { display: flex }");
    ck(of("nav").display == CSS_DISP_FLEX, "display: flex");
    sheet("nav { display: inline-flex }");
    ck(of("nav").display == CSS_DISP_FLEX, "inline-flex arranges the same way");
    sheet("main { display: grid }");
    ck(of("main").display == CSS_DISP_GRID, "display: grid");
    sheet("b { display: inline-block }");
    ck(of("b").display == CSS_DISP_INLINE, "inline-block stays inline");

    sheet("nav { display: flex; flex-direction: column }");
    ck(of("nav").fdir == CSS_FD_COLUMN, "flex-direction: column");
    sheet("nav { display: flex; flex-direction: row-reverse }");
    ck(of("nav").fdir == CSS_FD_ROW, "row-reverse is still a row");
    sheet("nav { display: flex; flex-flow: column wrap }");
    ck(of("nav").fdir == CSS_FD_COLUMN, "the flex-flow shorthand");

    sheet("main { display: grid; grid-template-columns: 1fr 1fr 1fr }");
    ck(of("main").cols == 3, "three named tracks");
    sheet("main { grid-template-columns: repeat(4, minmax(0, 1fr)) }");
    ck(of("main").cols == 4, "repeat(4, ...)");
    sheet("main { grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)) }");
    ck(of("main").cols == 0, "auto-fit leaves the count to us");
    sheet("main { grid-template-columns: none }");
    ck(of("main").cols == 0, "none asks for no tracks");

    sheet("nav { gap: 12px }");
    ck(of("nav").gap == 12, "gap");
    sheet("nav { column-gap: 8px }");
    ck(of("nav").gap == 8, "column-gap");
    sheet("nav { display: flex }");
    ck(of("nav").gap == -1, "and no gap unless the page asked for one");

    /* The direction and the column count come from rules that know nothing
     * of each other, so neither may silence the other. */
    sheet("nav.bar { flex-direction: column } nav { gap: 4px }");
    ck(of("nav.bar").fdir == CSS_FD_COLUMN && of("nav.bar").gap == 4,
       "a specific direction does not swallow a general gap");

    sheet("span { visibility: hidden }");
    ck(of("span").vis == CSS_V_HIDDEN, "visibility: hidden");
    sheet(".p { visibility: hidden } .p.open { visibility: visible }");
    ck(of("div.p").vis == CSS_V_HIDDEN, "hidden while it is closed");
    ck(of("div.p.open").vis == CSS_V_VISIBLE, "and shown again when opened");

    sheet("p { margin: 10px 20px 30px 40px }");
    ck(of("p").mt == 10 && of("p").mb == 30, "the margin shorthand");
    sheet("p { margin: 8px }");
    ck(of("p").mt == 8 && of("p").mb == 8, "one-value margin");

    sheet("h1 { font-size: 2em } small { font-size: 11px }");
    ck(of("h1").scale == 2, "font-size in em");
    ck(of("small").scale == 1, "a small font-size");

    /* --- things that must still be refused --------------------------------- */
    printf("refusals\n");
    sheet("a + b { color: #FF0000 } b { color: #00FF00 }");
    ck(coloris("a b", 0x00FF00), "the sibling combinator is dropped whole");

    /* --- a realistic sheet ------------------------------------------------- */
    printf("a page's worth\n");
    sheet(
        ":root { --x: 1 }\n"
        "body { color: #24292f; background: #ffffff; margin: 0 }\n"
        "@media (prefers-color-scheme: dark) { body { color: #c9d1d9 } }\n"
        ".Header { background-color: #161b22 }\n"
        ".Header a, .Header .link { color: #f0f6fc; text-decoration: none }\n"
        "nav.menu > ul > li a:hover { color: #58a6ff }\n"
        "h1, h2, h3 { font-weight: 600 }\n"
        "[data-view=\"grid\"] .item { display: none }\n"
        ".btn.btn-primary { background-color: #238636; color: #ffffff !important }\n");
    ck(css_nrules >= 9, "the sheet parsed into rules");
    ck(coloris("html body", 0xC9D1D9), "the dark-scheme override applied");
    ck(coloris("body div.Header a", 0xF0F6FC), "a two-part descendant");
    ck(of("body div.Header a").decor == CSS_D_NONE, "and its decoration");
    ck(of("body h2").weight == CSS_W_BOLD, "a selector list");
    ck(of("div@data-view=\"grid\" span.item").display == CSS_DISP_NONE,
       "attribute selector with a descendant");
    ck(coloris("a.btn.btn-primary", 0xFFFFFF), "two classes on one compound");


    /* --- custom properties -------------------------------------------------
     * The reason all of the above matters on a real page: a modern stylesheet
     * has almost no literal colours in it. GitHub's theme sheet is 87KB and
     * holds two; the other six hundred are var(--something). */
    printf("custom properties\n");
    set_root("");
    sheet(":root { --fg: #AA1122 } p { color: var(--fg) }");
    ck(coloris("p", 0xAA1122), "var() resolved");

    sheet(":root { --a: #223344 } p { color: var(--missing, #556677) }");
    ck(coloris("p", 0x556677), "the fallback when the name is unknown");

    sheet(":root { --base: #778899; --fg: var(--base) } p { color: var(--fg) }");
    ck(coloris("p", 0x778899), "a variable naming another variable");

    sheet("p { color: var(--nope) }");
    ck(nocolor("p"), "an unknown name with no fallback sets nothing");

    sheet(":root { --x: #010203 } @media screen { p { color: var(--x) } }");
    ck(coloris("p", 0x010203), "a variable used inside a media query");

    sheet(":root { --bg: #123456 } div { background-color: var(--bg) }");
    ck(of("div").has_bg && (of("div").bg & 0xFFFFFF) == 0x123456,
       "a variable behind a background");

    /* Definitions in a rule that does not reach the root are not collected,
     * which is what keeps one theme's palette out of another's. */
    sheet("[data-theme=dark] { --fg: #111111 } "
          "[data-theme=light] { --fg: #EEEEEE } "
          "p { color: var(--fg) }");
    ck(nocolor("p"), "no theme chosen: neither palette applies");

    set_root("data-theme=\"dark\"");
    sheet("[data-theme=dark] { --fg: #111111 } "
          "[data-theme=light] { --fg: #EEEEEE } "
          "p { color: var(--fg) }");
    ck(coloris("p", 0x111111), "the root's attributes choose the palette");

    set_root("data-theme=\"light\"");
    sheet("[data-theme=dark] { --fg: #111111 } "
          "[data-theme=light] { --fg: #EEEEEE } "
          "p { color: var(--fg) }");
    ck(coloris("p", 0xEEEEEE), "and the other one when it says so");

    /* prefers-color-scheme follows the ground we are drawing on. */
    set_root("");
    css_dark = 1;
    sheet("@media (prefers-color-scheme: dark) { p { color: #0D1117 } }"
          "@media (prefers-color-scheme: light) { p { color: #FFFFFF } }");
    ck(coloris("p", 0x0D1117), "a dark window takes the dark scheme");
    css_dark = 0;
    sheet("@media (prefers-color-scheme: dark) { p { color: #0D1117 } }"
          "@media (prefers-color-scheme: light) { p { color: #FFFFFF } }");
    ck(coloris("p", 0xFFFFFF), "and a light window the light one");
    css_dark = 1;
    set_root("");

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
