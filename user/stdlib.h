#ifndef NEPTUNE_STDLIB_H
#define NEPTUNE_STDLIB_H

#include "libc.h"

long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
char *getenv(const char *name);
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));

#endif
