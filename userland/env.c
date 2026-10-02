/* env -- the environment, one NAME=VALUE a line; or, with NAME=VALUE words
 * and a command after them, that command run with those set. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) {
        char *eq = strchr(argv[i], '=');
        *eq = 0;
        setenv(argv[i], eq + 1, 1);
        *eq = '=';
    }
    if (i >= argc) {
        for (char **e = environ; e && *e; e++) printf("%s\n", *e);
        return 0;
    }
    char path[512];
    if (strchr(argv[i], '/')) snprintf(path, sizeof path, "%s", argv[i]);
    else snprintf(path, sizeof path, "/bin/%s", argv[i]);
    int pid = spawn(path, argv + i, argc - i);
    if (pid < 0) { printf("env: %s: not found\n", argv[i]); return 127; }
    return waitpid(pid);
}
