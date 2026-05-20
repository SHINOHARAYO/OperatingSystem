#include "malloc.h"
#include "lib.h"


typedef struct block_meta {
    size_t size;
    struct block_meta *next;
    int free;
} block_meta_t;

#define META_SIZE sizeof(block_meta_t)
#define PAGE_SIZE 4096

static block_meta_t *global_base = 0;

static block_meta_t *find_free_block(block_meta_t **last, size_t size) {
    block_meta_t *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

static block_meta_t *request_space(block_meta_t *last, size_t size) {
    size_t total_needed = size + META_SIZE;
    size_t pages_needed = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t allocation_size = pages_needed * PAGE_SIZE;

    void *request = sys_mmap(allocation_size);
    if ((uint64_t)request == 0) {
        return 0;
    }

    block_meta_t *block = (block_meta_t *)request;
    block->size = allocation_size - META_SIZE;
    block->next = 0;
    block->free = 0;

    if (last) {
        last->next = block;
    }
    return block;
}

void* malloc(size_t size) {
    if (size == 0) return 0;
    size = (size + 7) & ~7;

    block_meta_t *block;

    if (!global_base) {
        block = request_space(0, size);
        if (!block) return 0;
        global_base = block;
    } else {
        block_meta_t *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (!block) return 0;
        } else {
            block->free = 0;
        }
    }
    return (block + 1);
}

static block_meta_t *get_block_ptr(void *ptr) {
    return (block_meta_t *)ptr - 1;
}

void free(void *ptr) {
    if (!ptr) return;

    block_meta_t *block_ptr = get_block_ptr(ptr);
    block_ptr->free = 1;
    block_meta_t *current = global_base;
    while (current != 0 && current->next != 0) {
        if (current->free && current->next->free) {
            current->size += META_SIZE + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}
