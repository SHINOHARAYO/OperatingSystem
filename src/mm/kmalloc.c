#include "kmalloc.h"
#include "pmm.h"
#include "mmu.h"
#include "vmm.h"
#include "uart.h"
#include "log.h"

#define HEAP_START_VA 0xFFFFFF8000000000ULL
#define HEAP_SIZE     (2 * 1024 * 1024)

static uint64_t heap_current_va = HEAP_START_VA;
static uint64_t heap_mapped_va = HEAP_START_VA;

static inline uint64_t align_up(uint64_t addr, uint64_t align) {
    return (addr + align - 1) & ~(align - 1);
}

void kmalloc_init(void) {
    LOG_INFO_HEX("KMALLOC: Initializing Kernel Heap (Bump Allocator) at ", HEAP_START_VA);
}

void *kmalloc(uint64_t size) {
    if (size == 0) return NULL;

    uint64_t alloc_size = align_up(size, 16);

    if (heap_current_va + alloc_size > HEAP_START_VA + HEAP_SIZE) {
        LOG_FAIL("KMALLOC OOM: Heap Exhausted!");
        return NULL;
    }

    uint64_t allocated_ptr = heap_current_va;
    
    while (heap_mapped_va <= heap_current_va + alloc_size) {
        void *phys_page = pmm_alloc_page();
        if (!phys_page) {
            LOG_FAIL("KMALLOC OOM: Physical memory exhausted backing heap!");
            return NULL;
        }
        
        vmm_map_page(NULL, heap_mapped_va, (uint64_t)phys_page, VMM_FLAG_READWRITE);
        heap_mapped_va += 4096;
    }

    heap_current_va += alloc_size;
    
    uint8_t *mem = (uint8_t *)allocated_ptr;
    for(uint64_t i = 0; i < alloc_size; i++) {
        mem[i] = 0;
    }

    return (void *)allocated_ptr;
}

void kfree(void *ptr) {
    // A bump allocator cannot free memory individually.
    (void)ptr;
}

uint64_t kmalloc_get_used(void) {
    return heap_current_va - HEAP_START_VA;
}

uint64_t kmalloc_get_mapped(void) {
    return heap_mapped_va - HEAP_START_VA;
}

uint64_t kmalloc_get_size(void) {
    return HEAP_SIZE;
}
