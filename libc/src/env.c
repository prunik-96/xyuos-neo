/* The environment: NAME=VALUE strings every process has, a copy of its
 * parent's. The kernel keeps each process's block (SYS_ENV) so a child gets
 * it at birth; here it is read in once and kept as the usual vector.
 * setenv() writes the block back, so what a shell exports, the programs it
 * starts afterwards see. */

#include <stdlib.h>
#include <string.h>
#include "xyuos_syscall.h"

static char *env_empty[1] = { NULL };
char **environ = env_empty;

static char *block;            /* "A=1\0B=2\0" */
static unsigned blen;
static char **vec;
static int loaded;

static void rebuild_vec(void) {
    int n = 0;
    for (unsigned i = 0; i < blen; i += (unsigned)strlen(block + i) + 1) n++;
    char **v = (char **)malloc(sizeof(char *) * (size_t)(n + 1));
    if (!v) return;
    int k = 0;
    for (unsigned i = 0; i < blen; i += (unsigned)strlen(block + i) + 1)
        if (block[i]) v[k++] = block + i;
    v[k] = NULL;
    free(vec);
    vec = v;
    environ = vec;
}

static void env_load(void);
void __env_init(void) { env_load(); }

static void env_load(void) {
    if (loaded) return;
    loaded = 1;
    long n = xyuos_syscall3(SYS_ENV, ENVOP_GET, 0, 0);
    if (n <= 0) return;
    block = (char *)malloc((size_t)n + 1);
    if (!block) return;
    long got = xyuos_syscall3(SYS_ENV, ENVOP_GET, (long)block, n);
    if (got < 0) { free(block); block = 0; return; }
    blen = (unsigned)(got < n ? got : n);
    block[blen] = 0;
    rebuild_vec();
}

static int name_is(const char *entry, const char *name, size_t nl) {
    return strncmp(entry, name, nl) == 0 && entry[nl] == '=';
}

char *getenv(const char *name) {
    env_load();
    size_t nl = strlen(name);
    for (unsigned i = 0; i < blen; i += (unsigned)strlen(block + i) + 1)
        if (name_is(block + i, name, nl)) return block + i + nl + 1;
    return NULL;
}

/* The block without `name`, and with NAME=VALUE at its end if value is set. */
static int env_put(const char *name, const char *value) {
    env_load();
    size_t nl = strlen(name), vl = value ? strlen(value) : 0;
    if (!nl || strchr(name, '=')) return -1;
    unsigned cap = blen + (unsigned)(nl + vl + 2) + 1;
    char *nb = (char *)malloc(cap);
    if (!nb) return -1;
    unsigned o = 0;
    for (unsigned i = 0; i < blen; i += (unsigned)strlen(block + i) + 1) {
        size_t l = strlen(block + i);
        if (!l || name_is(block + i, name, nl)) continue;
        memcpy(nb + o, block + i, l + 1);
        o += (unsigned)l + 1;
    }
    if (value) {
        memcpy(nb + o, name, nl);
        nb[o + nl] = '=';
        memcpy(nb + o + nl + 1, value, vl + 1);
        o += (unsigned)(nl + vl + 2);
    }
    free(block);
    block = nb;
    blen = o;
    xyuos_syscall3(SYS_ENV, ENVOP_SET, (long)block, blen);
    rebuild_vec();
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return env_put(name, value ? value : "");
}

int unsetenv(const char *name) { return env_put(name, NULL); }

int putenv(char *s) {
    char *eq = strchr(s, '=');
    if (!eq) return unsetenv(s);
    char name[128];
    size_t nl = (size_t)(eq - s);
    if (nl >= sizeof name) return -1;
    memcpy(name, s, nl);
    name[nl] = 0;
    return env_put(name, eq + 1);
}

int clearenv(void) {
    env_load();
    free(block);
    block = 0;
    blen = 0;
    xyuos_syscall3(SYS_ENV, ENVOP_SET, 0, 0);
    rebuild_vec();
    return 0;
}
