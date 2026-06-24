#ifndef NEPTUNE_UNISTD_H
#define NEPTUNE_UNISTD_H

#include "libc.h"

int unlink(const char *path);
char *getcwd(char *buffer, size_t size);
char *realpath(const char *path, char *resolved);

#endif
