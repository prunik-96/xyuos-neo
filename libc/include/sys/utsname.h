/* What system this is. */
#ifndef SYS_UTSNAME_H
#define SYS_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

#ifdef __cplusplus
extern "C" {
#endif

/* Always succeeds, and always says the same thing: there is one system here
 * and it does not change its name between calls. */
int uname(struct utsname *out);

#ifdef __cplusplus
}
#endif

#endif
