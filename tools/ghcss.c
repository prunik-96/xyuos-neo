/* Feed the engine GitHub's real stylesheets and ask it about the one element
 * that decides whether the page reads as a menu or as a page. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

typedef struct { unsigned win, text; } fake_pal;
static fake_pal gui_pal = { 0x111418, 0xD8DEE9 };
#define GC_WIN  (gui_pal.win)
#define GC_TEXT (gui_pal.text)

#include "../userland/css.h"

static char buf[4 * 1024 * 1024];

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/gh";
    int limit = argc > 2 ? atoi(argv[2]) : 24;

    css_init();

    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    long total = 0;
    while (d && (e = readdir(d))) {
        int l = (int)strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".css")) continue;
        if (n >= limit) { printf("  (stopping at %d sheets)\n", limit); break; }
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        long got = (long)fread(buf, 1, sizeof buf, f);
        fclose(f);
        css_add_source(buf, (int)got);
        total += got;
        n++;
    }
    if (d) closedir(d);
    printf("%d sheets, %ld bytes, buffered %d of %d\n",
           n, total, css_srclen, CSS_SRC_CAP);

    /* The root as GitHub declares it. */
    css_elem root;
    memset(&root, 0, sizeof root);
    root.tag = "html";
    root.attr = "lang=\"en\" data-color-mode=\"dark\" data-dark-theme=\"dark\"";
    root.attrlen = (int)strlen(root.attr);
    root.first = root.last = 1;

    css_viewport_w = 1150;
    css_dark = 1;
    css_build(&root, 1);
    printf("rules kept: %d of %d, variables: %d\n",
           css_nrules, CSS_MAX_RULES, css_var_n);

    /* The list the six menu buttons hang off: a row, or the page reads as
     * six headings stacked down the left. */
    css_elem ul[3];
    memset(ul, 0, sizeof ul);
    ul[0] = root;
    ul[1].tag = "nav"; ul[1].cls = "MarketingNavigation-module__nav__W0KYY";
    ul[1].first = ul[1].last = 1;
    ul[2].tag = "ul"; ul[2].cls = "MarketingNavigation-module__list__tFbMb";
    ul[2].first = ul[2].last = 1;
    {
        css_style_t u;
        css_style_init(&u);
        css_match_chain(ul, 3, &u);
        printf("the nav list:               display=%d (4 = flex) fdir=%d gap=%d\n",
               u.display, u.fdir, u.gap);
    }

    /* The dropdown panel that holds the whole mega-menu. */
    css_elem chain[4];
    memset(chain, 0, sizeof chain);
    chain[0] = root;
    chain[1].tag = "nav"; chain[1].cls = "MarketingNavigation-module__nav__W0KYY";
    chain[1].first = chain[1].last = 1;
    chain[2].tag = "div"; chain[2].cls = "NavDropdown-module__container__l2YeI";
    chain[2].first = chain[2].last = 1;
    chain[3].tag = "div"; chain[3].cls = "NavDropdown-module__dropdown__xm1jd";
    chain[3].first = chain[3].last = 1;

    /* Where in the chain the hiding is declared decides whether a subtree
     * skip is enough, or whether a descendant has to be able to undo it. */
    for (int d = 1; d < 4; d++) {
        css_style_t a;
        css_style_init(&a);
        css_match_chain(chain, d + 1, &a);
        printf("  depth %d <%s class=\"%.40s\">  display=%d vis=%d\n",
               d, chain[d].tag, chain[d].cls ? chain[d].cls : "", a.display, a.vis);
    }

    css_style_t st;
    css_style_init(&st);
    css_match_chain(chain, 4, &st);
    printf("\nthe dropdown panel:         display=%d vis=%d (2 = hidden)\n",
           st.display, st.vis);

    /* And the same panel when the menu really is open. */
    chain[2].cls = "NavDropdown-module__container__l2YeI open";
    css_style_init(&st);
    css_match_chain(chain, 4, &st);
    printf("with .open on its container: display=%d vis=%d\n",
           st.display, st.vis);
    return 0;
}
