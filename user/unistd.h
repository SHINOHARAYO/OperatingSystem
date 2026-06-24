#ifndef NEPTUNE_UNISTD_H
#define NEPTUNE_UNISTD_H

#include "libc.h"

int open(const char *path, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
char *getcwd(char *buffer, size_t size);
char *realpath(const char *path, char *resolved);

#endif
