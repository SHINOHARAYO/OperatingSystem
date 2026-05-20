#pragma once

#include <stdint.h>
#include <stddef.h>

void kmalloc_init(void);

void *kmalloc(uint64_t size);

void kfree(void *ptr);

uint64_t kmalloc_get_used(void);
uint64_t kmalloc_get_mapped(void);
uint64_t kmalloc_get_size(void);
