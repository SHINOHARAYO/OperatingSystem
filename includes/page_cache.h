#pragma once

#include <stdint.h>

int page_cache_init(void);
int page_cache_get_initrd_page(uint32_t file_index, uint64_t file_offset,
                               uint64_t *out_paddr);

