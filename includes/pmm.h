#pragma once

#include <stdint.h>
#include <stddef.h>

#define EFI_RESERVED_MEMORY_TYPE 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_RUNTIME_SERVICES_CODE 5
#define EFI_RUNTIME_SERVICES_DATA 6
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_UNUSABLE_MEMORY 8
#define EFI_ACPI_RECLAIM_MEMORY 9
#define EFI_ACPI_MEMORY_NVS 10
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_PAL_CODE 13
#define EFI_PERSISTENT_MEMORY 14

void pmm_init(void *memory_map, uint64_t map_size, uint64_t desc_size);
void *pmm_alloc_page(void);
void *pmm_alloc_contiguous_pages(uint64_t page_count);
void pmm_free_page(void *page);
void pmm_free_contiguous_pages(void *pages, uint64_t page_count);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_page_count(void);
int pmm_page_index(uint64_t paddr, uint64_t *index);
uint64_t pmm_page_from_index(uint64_t index);
