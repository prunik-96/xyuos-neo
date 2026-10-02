/* notify -- put a notice on the desktop.
 *
 *     notify "Title" "a line of text"
 *
 * It stays a few seconds in the corner of the screen and goes away by itself
 * (or at once, clicked). Nothing is shown while "do not disturb" is on. */

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: notify TITLE [TEXT]\n");
        return 1;
    }
    if (notify(argv[1], argc > 2 ? argv[2] : "") != 0) {
        printf("notify: no desktop to show it on\n");
        return 1;
    }
    return 0;
}
