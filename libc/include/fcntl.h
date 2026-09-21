#ifndef FCNTL_H
#define FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_EXCL    0200
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_BINARY  0        /* no text/binary distinction here */

int open(const char *path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
