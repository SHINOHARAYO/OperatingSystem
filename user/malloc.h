#ifndef MALLOC_H
#define MALLOC_H

#include <stdint.h>
#include <stddef.h>

void* malloc(size_t size);
void free(void* ptr);

#endif
