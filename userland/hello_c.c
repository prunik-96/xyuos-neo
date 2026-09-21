#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("Hello from libc on xyuOS Neo!\n");
    printf("2+2=%d\n", 2 + 2);

    char *buf = (char *)malloc(64);
    strcpy(buf, "malloc/strcpy/free work too.");
    printf("%s\n", buf);
    free(buf);

    FILE *f = fopen("/hello.txt", "r");
    if (f) {
        printf("-- reading /hello.txt via fopen/fread --\n");
        char chunk[128];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk) - 1, f)) > 0) {
            chunk[n] = '\0';
            printf("%s", chunk);
        }
        fclose(f);
    } else {
        printf("fopen failed\n");
    }

    return 0;
}
