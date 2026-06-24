#ifndef NEPTUNE_ERRNO_H
#define NEPTUNE_ERRNO_H

extern int errno;

#define EINTR 4
#define ENOENT 2
#define EINVAL 22
#define ENOMEM 12

char *strerror(int error);

#endif
