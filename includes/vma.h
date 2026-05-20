#pragma once

#include <stdint.h>
#include <stddef.h>

#define VMA_USER_LIMIT (1ULL << 39)

#define VMA_READ       (1ULL << 0)
#define VMA_WRITE      (1ULL << 1)
#define VMA_EXEC       (1ULL << 2)
#define VMA_USER       (1ULL << 3)
#define VMA_GROWS_DOWN (1ULL << 4)
#define VMA_LAZY       (1ULL << 5)
#define VMA_FILE       (1ULL << 6)
#define VMA_GUARD      (1ULL << 7)
#define VMA_STACK      (1ULL << 8)
#define VMA_MMAP       (1ULL << 9)
#define VMA_ELF        (1ULL << 10)

#define VMA_BACKING_NONE   0
#define VMA_BACKING_ANON   1
#define VMA_BACKING_FILE   2
#define VMA_BACKING_DEVICE 3

typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t committed_pages;
    uint64_t max_pages;
    uint32_t file_cap;
    uint32_t backing_type;
    uint32_t object_id;
    uint32_t reserved;
} vma_t;

typedef struct {
    vma_t *vmas;
    uint32_t count;
    uint32_t capacity;
    uint32_t table_pages;
    int32_t last_vma;
} vm_space_t;

int vma_init(vm_space_t *vm);
void vma_destroy(vm_space_t *vm);
int vma_add(vm_space_t *vm, uint64_t start, uint64_t end, uint64_t flags,
            uint64_t file_offset, uint64_t file_size, uint32_t file_cap);
int vma_add_ex(vm_space_t *vm, uint64_t start, uint64_t end, uint64_t flags,
               uint64_t file_offset, uint64_t file_size, uint32_t file_cap,
               uint32_t backing_type, uint32_t object_id,
               uint64_t committed_pages, uint64_t max_pages);
int vma_remove(vm_space_t *vm, uint64_t start, uint64_t end);
void vma_coalesce(vm_space_t *vm);
vma_t *vma_find(vm_space_t *vm, uint64_t addr);
vma_t *vma_next(vm_space_t *vm, const vma_t *vma);
int vma_range_is_free(const vm_space_t *vm, uint64_t start, uint64_t end);
int vma_check(vm_space_t *vm, uint64_t addr, uint64_t required_flags);
const vma_t *vma_get(const vm_space_t *vm, uint32_t index);
