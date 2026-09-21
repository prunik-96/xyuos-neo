#ifndef ERRNO_H
#define ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

// Single-process OS, so a plain global is enough (no per-thread errno).
extern int errno;

// The whole low range, with the values Linux uses. Most of these are never
// set by anything here -- there are no child processes, no block devices, no
// sockets -- but ported code TESTS for the names, and a program that will not
// compile is worse than one that checks for an error that cannot happen.
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define ENXIO         6
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define EAGAIN       11
#define EWOULDBLOCK  EAGAIN
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define ENOTBLK      15
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define ETXTBSY      26
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EMLINK       31
#define EPIPE        32
#define EDOM         33
#define ERANGE       34
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ELOOP        40

// The network range, for code that expects it to exist.
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY    114
#define EINPROGRESS 115
#define ENOTSUP      95
#define EOVERFLOW    75
#define EILSEQ       84
#define ECANCELED   125

#ifdef __cplusplus
}
#endif

#endif
