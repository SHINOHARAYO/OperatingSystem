#ifndef NEPTUNE_STRING_H
#define NEPTUNE_STRING_H

#include "libc.h"

char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strpbrk(const char *s, const char *accept);
char *strdup(const char *s);
int strcasecmp(const char *a, const char *b);

#endif
