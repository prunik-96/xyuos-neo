#ifndef SYS_IOCTL_H
#define SYS_IOCTL_H

#define FIONREAD 0x541B          /* int *: bytes waiting on a socket */
#define FIONBIO  0x5421          /* int *: non-zero makes a socket non-blocking */

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long req, ...);

#ifdef __cplusplus
}
#endif

#endif
