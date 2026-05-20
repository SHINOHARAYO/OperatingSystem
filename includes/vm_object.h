#pragma once

#include <stdint.h>

#define MAX_VM_OBJECTS 128

#define VM_OBJECT_NONE  0
#define VM_OBJECT_ANON  1
#define VM_OBJECT_INITRD 2

typedef struct {
    int present;
    uint32_t type;
    uint32_t refcount;
    uint32_t owner_tid;
    uint32_t file_index;
    uint64_t file_offset;
    uint64_t size;
    uint64_t data_size;
    uint64_t page_count;
    uint64_t flags;
} vm_object_t;

int vm_object_init(void);
int vm_object_create_anon(uint32_t owner_tid, uint64_t size, uint64_t flags,
                          uint32_t *out_id);
int vm_object_create_initrd(uint32_t owner_tid, uint32_t file_index,
                            uint64_t file_offset, uint64_t data_size,
                            uint64_t object_size,
                            uint64_t flags, uint32_t *out_id);
int vm_object_ref(uint32_t object_id);
int vm_object_unref(uint32_t object_id);
const vm_object_t *vm_object_get(uint32_t object_id);
int vm_object_resolve_page(uint32_t object_id, uint64_t object_offset,
                           uint32_t owner_tid, uint64_t *out_paddr);
