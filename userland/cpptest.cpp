/* C++ with the language switched all the way on: objects that are built
 * before main and torn down after it, exceptions that unwind the stack, and
 * types a program can ask about at run time.
 *
 * The test with teeth is the third. Throwing from three frames down must run
 * the destructor of every object those frames own, in the right order, and
 * land in the right catch -- and if any of that is missing the program does
 * not fail to link, it leaks quietly or stops dead. The trace string is what
 * makes the difference visible.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exception>
#include <new>
#include <typeinfo>

static int bad = 0;
static char trace[256];
static int  trace_n = 0;

static void note(const char *s) {
    for (const char *p = s; *p && trace_n < (int)sizeof trace - 1; p++)
        trace[trace_n++] = *p;
    trace[trace_n] = 0;
}

/* --- built before main, torn down after it ------------------------------- */

struct Early {
    Early()  { note("E+"); }
    ~Early() { printf("ok: the file-scope object was destroyed after main\n"); }
};
static Early early;              /* its constructor runs before main */

/* --- what unwinding has to undo ------------------------------------------ */

struct Marker {
    const char *tag;
    explicit Marker(const char *t) : tag(t) { note(tag); }
    ~Marker() { note("~"); note(tag); }
};

struct Trouble {
    int code;
    explicit Trouble(int c) : code(c) {}
};

static void deepest() {
    Marker m("c");
    throw Trouble(7);            /* m must be destroyed on the way out */
}

static void middle() {
    Marker m("b");
    deepest();
    note("!never");              /* unreachable: the throw goes past here */
}

static void outer() {
    Marker m("a");
    middle();
    note("!never");
}

/* --- types at run time ---------------------------------------------------- */

struct Shape          { virtual ~Shape() {} virtual int sides() const = 0; };
struct Square : Shape { int sides() const override { return 4; } };
struct Circle : Shape { int sides() const override { return 0; } };

int main() {
    printf("-- C++: constructors, exceptions, RTTI --\n");

    /* --- 1. the file-scope object was built before main ------------------ */
    if (strcmp(trace, "E+") != 0) {
        printf("FAILED: the file-scope constructor did not run (trace \"%s\")\n", trace);
        bad++;
    } else {
        printf("ok: a file-scope object was constructed before main\n");
    }
    trace_n = 0; trace[0] = 0;

    /* --- 2. a static local, built the first time it is reached ----------- */
    {
        struct Once { Once() { note("O"); } };
        for (int i = 0; i < 3; i++) { static Once once; (void)once; }
        if (strcmp(trace, "O") != 0) {
            printf("FAILED: a static local was built %d times (trace \"%s\")\n",
                   (int)strlen(trace), trace);
            bad++;
        } else {
            printf("ok: a static local was built exactly once\n");
        }
    }
    trace_n = 0; trace[0] = 0;

    /* --- 3. throwing through three frames -------------------------------- */
    try {
        outer();
        printf("FAILED: the throw did not leave outer()\n");
        bad++;
    } catch (const Trouble &t) {
        if (t.code != 7) { printf("FAILED: caught code %d, not 7\n", t.code); bad++; }
        /* a b c, then c b a undone: each Marker destroyed as its frame goes */
        if (strcmp(trace, "abc~c~b~a") != 0) {
            printf("FAILED: unwinding went \"%s\", not \"abc~c~b~a\"\n", trace);
            bad++;
        } else {
            printf("ok: threw three frames down, caught it, and every "
                   "destructor ran in order\n");
        }
    } catch (...) {
        printf("FAILED: caught something that was not a Trouble\n");
        bad++;
    }

    /* --- 4. the right catch, and rethrow --------------------------------- */
    try {
        try {
            throw std::bad_alloc();
        } catch (const Trouble &) {
            printf("FAILED: a bad_alloc was caught as a Trouble\n");
            bad++;
        } catch (const std::exception &) {
            throw;                       /* pass it on */
        }
        printf("FAILED: the rethrow went nowhere\n");
        bad++;
    } catch (const std::bad_alloc &) {
        printf("ok: the matching handler ran, and a rethrow reached the next\n");
    }

    /* --- 5. what a type is at run time ------------------------------------ */
    {
        Shape *s = new Square();
        Shape *c = new Circle();

        if (dynamic_cast<Square *>(s) == nullptr) {
            printf("FAILED: dynamic_cast to the right type failed\n"); bad++;
        } else if (dynamic_cast<Square *>(c) != nullptr) {
            printf("FAILED: dynamic_cast to the wrong type succeeded\n"); bad++;
        } else if (typeid(*s) == typeid(*c)) {
            printf("FAILED: two different types compare equal\n"); bad++;
        } else {
            printf("ok: dynamic_cast and typeid tell %s from %s\n",
                   typeid(*s).name(), typeid(*c).name());
        }
        delete s;
        delete c;
    }

    /* --- 6. new that fails throws, and is catchable ----------------------- */
    {
        bool threw = false;
        try {
            /* More than the whole window, so it cannot be satisfied however
             * much memory the machine has. */
            volatile char *p = new char[3000000000UL];
            (void)p;
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        if (!threw) { printf("FAILED: a new that cannot be satisfied returned\n"); bad++; }
        else printf("ok: new threw bad_alloc rather than handing back nothing\n");
    }

    printf(bad ? "FAILED\n" : "all good\n");
    /* The file-scope destructor prints after this line -- which is the point
     * of it, so returning rather than _exit()ing matters. */
    return bad ? 1 : 0;
}
