/* Compiled ON xyuOS Neo, by xyuOS Neo's own C compiler.
 * Try: cc /demo.c -o /bin/demo   then just: demo */

#include <stdio.h>

int fib(int n) {
    return (n < 2) ? n : fib(n - 1) + fib(n - 2);
}

int main(int argc, char **argv) {
    printf("compiled and run on xyuOS Neo\n");

    printf("fib:");
    for (int i = 0; i < 10; i++) printf(" %d", fib(i));
    printf("\n");

    printf("argc=%d", argc);
    for (int i = 0; i < argc; i++) printf(" argv[%d]=%s", i, argv[i]);
    printf("\n");

    return 0;
}
