/* stat -- show what the filesystem knows about a file. */

#include <stdio.h>
#include <unistd.h>
#include "upath.h"

/* Unix seconds (UTC) -> "YYYY-MM-DD HH:MM:SS" (Howard Hinnant's civil_from_days). */
static void fmt_time(unsigned int t, char *out) {
    long days = (long)t / 86400;
    int rem = (int)((long)t % 86400);
    int hh = rem / 3600, mm = (rem % 3600) / 60, ss = rem % 60;
    days += 719468;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    long doe = days - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    sprintf(out, "%04ld-%02ld-%02ld %02d:%02d:%02d UTC", y, m, d, hh, mm, ss);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: stat FILE...\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        char path[UPATH_MAX];
        upath_resolve("/", argv[i], path);

        struct xyuos_stat st;
        if (xyuos_stat(path, &st) != 0) {
            printf("stat: cannot stat '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        printf("%s\n", path);
        printf("  type:  %s\n", st.is_dir ? "directory" : "regular file");
        printf("  size:  %u bytes\n", (unsigned)st.size);
        printf("  inode: %u\n", (unsigned)st.inode);
        if (st.mtime) {
            char tb[40];
            fmt_time(st.mtime, tb);
            printf("  mtime: %s\n", tb);
        } else {
            printf("  mtime: (unknown)\n");
        }
    }
    return status;
}
