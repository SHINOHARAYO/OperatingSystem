#include "pmm.h"
#include "efi.h"
#include "mmu.h"
#include "uart.h"
#include "log.h"
static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t usable_pages = 0;
static uint64_t free_pages = 0;
static uint64_t memory_offset = 0;
static uint64_t next_free_hint = 0;
static void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}
static void bitmap_clear(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}
static int bitmap_test(uint64_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(void *memory_map, uint64_t map_size, uint64_t desc_size) {
    LOG_INFO("PMM: Parsing UEFI Memory Map...");

    uint64_t highest_address = 0;
    uint64_t lowest_address = 0xFFFFFFFFFFFFFFFF;
    uint64_t largest_free_start = 0;
    uint64_t largest_free_pages = 0;
    for (uint64_t i = 0; i < map_size / desc_size; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)memory_map + (i * desc_size));
        
        if (desc->PhysicalStart < lowest_address) lowest_address = desc->PhysicalStart;
        if (desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE) > highest_address) {
            highest_address = desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE);
        }

        if (desc->Type == EFI_CONVENTIONAL_MEMORY) {
            if (desc->NumberOfPages > largest_free_pages) {
                largest_free_pages = desc->NumberOfPages;
                largest_free_start = desc->PhysicalStart;
            }
        }
    }

    memory_offset = lowest_address;
    total_pages = (highest_address - lowest_address) / PAGE_SIZE;
    usable_pages = 0;
    uint64_t bitmap_size_bytes = total_pages / 8;
    if (total_pages % 8) bitmap_size_bytes++;

    LOG_INFO_HEX("PMM: Total Ram Extent: ", highest_address - lowest_address);
    LOG_INFO_HEX("PMM: Bitmap Size needed (bytes): ", bitmap_size_bytes);
    if (largest_free_pages * PAGE_SIZE < bitmap_size_bytes) {
        LOG_FAIL("PMM PANIC: Not enough contiguous memory for Bitmap!");
        while(1);
    }

    bitmap = (uint8_t *)largest_free_start;

    for (uint64_t i = 0; i < bitmap_size_bytes; i++) {
        bitmap[i] = 0xFF;
    }
    free_pages = 0;
    for (uint64_t i = 0; i < map_size / desc_size; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)memory_map + (i * desc_size));
        
        if (desc->Type == EFI_CONVENTIONAL_MEMORY) {
            usable_pages += desc->NumberOfPages;
            uint64_t start_bit = (desc->PhysicalStart - memory_offset) / PAGE_SIZE;
            for (uint64_t j = 0; j < desc->NumberOfPages; j++) {
                bitmap_clear(start_bit + j);
                free_pages++;
            }
        }
    }
    uint64_t bitmap_start_bit = ((uint64_t)bitmap - memory_offset) / PAGE_SIZE;
    uint64_t bitmap_pages = (bitmap_size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        if (!bitmap_test(bitmap_start_bit + i)) {
            bitmap_set(bitmap_start_bit + i);
            free_pages--;
        }
    }

    next_free_hint = 0;
    while (next_free_hint < total_pages && bitmap_test(next_free_hint)) {
        next_free_hint++;
    }
    if (next_free_hint >= total_pages) {
        next_free_hint = 0;
    }

    LOG_OK_HEX("PMM: Initialized. Free Pages: ", free_pages);
}

void *pmm_alloc_page(void) {
    for (uint64_t searched = 0; searched < total_pages; searched++) {
        uint64_t i = next_free_hint + searched;
        if (i >= total_pages) {
            i -= total_pages;
        }
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            next_free_hint = i + 1;
            if (next_free_hint >= total_pages) {
                next_free_hint = 0;
            }
            uint64_t paddr = memory_offset + (i * PAGE_SIZE);
            
            uint8_t *page = (uint8_t *)paddr;
            for(int j = 0; j < PAGE_SIZE; j++) page[j] = 0;
            
            return (void *)paddr;
        }
    }
    LOG_FAIL("PMM OOM!");
    return NULL;
}

void *pmm_alloc_contiguous_pages(uint64_t page_count) {
    if (page_count == 0 || page_count > total_pages) {
        return NULL;
    }

    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (run_len == 0) {
                run_start = i;
            }
            run_len++;
            if (run_len == page_count) {
                for (uint64_t j = 0; j < page_count; j++) {
                    bitmap_set(run_start + j);
                }
                free_pages -= page_count;

                uint64_t paddr = memory_offset + (run_start * PAGE_SIZE);
                uint8_t *ptr = (uint8_t *)paddr;
                for (uint64_t b = 0; b < page_count * PAGE_SIZE; b++) {
                    ptr[b] = 0;
                }
                return (void *)paddr;
            }
        } else {
            run_len = 0;
        }
    }

    LOG_FAIL("PMM contiguous OOM!");
    return NULL;
}

void pmm_free_page(void *page) {
    uint64_t paddr = (uint64_t)page;
    if (paddr < memory_offset || paddr >= memory_offset + (total_pages * PAGE_SIZE)) return;
    
    uint64_t bit = (paddr - memory_offset) / PAGE_SIZE;
    if (bitmap_test(bit)) {
        bitmap_clear(bit);
        free_pages++;
        if (bit < next_free_hint) {
            next_free_hint = bit;
        }
    }
}

void pmm_free_contiguous_pages(void *pages, uint64_t page_count) {
    if (!pages || page_count == 0) {
        return;
    }

    uint64_t paddr = (uint64_t)pages;
    if (paddr < memory_offset ||
        paddr >= memory_offset + (total_pages * PAGE_SIZE) ||
        (paddr & (PAGE_SIZE - 1)) != 0) {
        return;
    }

    uint64_t start_bit = (paddr - memory_offset) / PAGE_SIZE;
    if (page_count > total_pages - start_bit) {
        return;
    }

    for (uint64_t i = 0; i < page_count; i++) {
        if (bitmap_test(start_bit + i)) {
            bitmap_clear(start_bit + i);
            free_pages++;
            if (start_bit + i < next_free_hint) {
                next_free_hint = start_bit + i;
            }
        }
    }
}

uint64_t pmm_get_free_memory(void) {
    return free_pages * PAGE_SIZE;
}

uint64_t pmm_get_total_memory(void) {
    return usable_pages * PAGE_SIZE;
}

uint64_t pmm_get_page_count(void) {
    return total_pages;
}

int pmm_page_index(uint64_t paddr, uint64_t *index) {
    if (!index || paddr < memory_offset ||
        paddr >= memory_offset + (total_pages * PAGE_SIZE) ||
        (paddr & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    *index = (paddr - memory_offset) / PAGE_SIZE;
    return 0;
}

uint64_t pmm_page_from_index(uint64_t index) {
    if (index >= total_pages) {
        return 0;
    }

    return memory_offset + (index * PAGE_SIZE);
}
