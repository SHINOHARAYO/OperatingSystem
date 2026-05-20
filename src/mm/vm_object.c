#include "vm_object.h"
#include "frame.h"
#include "initrd.h"
#include "mmu.h"
#include "page_cache.h"
#include "log.h"

static vm_object_t objects[MAX_VM_OBJECTS];

static uint64_t align_pages(uint64_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void clear_object(vm_object_t *object) {
    if (!object) {
        return;
    }

    object->present = 0;
    object->type = VM_OBJECT_NONE;
    object->refcount = 0;
    object->owner_tid = 0;
    object->file_index = 0;
    object->file_offset = 0;
    object->size = 0;
    object->data_size = 0;
    object->page_count = 0;
    object->flags = 0;
}

int vm_object_init(void) {
    for (uint32_t i = 0; i < MAX_VM_OBJECTS; i++) {
        clear_object(&objects[i]);
    }

    LOG_OK_HEX("VMOBJ: object slots: ", MAX_VM_OBJECTS);
    return 0;
}

static int allocate_object(uint32_t type, uint32_t owner_tid, uint64_t size,
                           uint64_t flags, uint32_t *out_id) {
    if (!out_id || type == VM_OBJECT_NONE || size == 0) {
        return -1;
    }

    for (uint32_t i = 1; i < MAX_VM_OBJECTS; i++) {
        if (!objects[i].present) {
            objects[i].present = 1;
            objects[i].type = type;
            objects[i].refcount = 1;
            objects[i].owner_tid = owner_tid;
            objects[i].file_index = 0;
            objects[i].file_offset = 0;
            objects[i].size = size;
            objects[i].data_size = size;
            objects[i].page_count = align_pages(size);
            objects[i].flags = flags;
            *out_id = i;
            return 0;
        }
    }

    return -1;
}

int vm_object_create_anon(uint32_t owner_tid, uint64_t size, uint64_t flags,
                          uint32_t *out_id) {
    return allocate_object(VM_OBJECT_ANON, owner_tid, size, flags, out_id);
}

int vm_object_create_initrd(uint32_t owner_tid, uint32_t file_index,
                            uint64_t file_offset, uint64_t data_size,
                            uint64_t object_size,
                            uint64_t flags, uint32_t *out_id) {
    const uint8_t *file_data = 0;
    uint64_t file_size = 0;
    if (initrd_get_file(file_index, &file_data, &file_size) < 0 ||
        !file_data || file_offset > file_size ||
        data_size > file_size - file_offset ||
        data_size > object_size) {
        return -1;
    }

    if (allocate_object(VM_OBJECT_INITRD, owner_tid, object_size, flags, out_id) < 0) {
        return -1;
    }

    objects[*out_id].file_index = file_index;
    objects[*out_id].file_offset = file_offset;
    objects[*out_id].data_size = data_size;
    return 0;
}

int vm_object_ref(uint32_t object_id) {
    if (object_id == 0) {
        return 0;
    }
    if (object_id >= MAX_VM_OBJECTS || !objects[object_id].present ||
        objects[object_id].refcount == UINT32_MAX) {
        return -1;
    }

    objects[object_id].refcount++;
    return 0;
}

int vm_object_unref(uint32_t object_id) {
    if (object_id == 0) {
        return 0;
    }
    if (object_id >= MAX_VM_OBJECTS || !objects[object_id].present ||
        objects[object_id].refcount == 0) {
        return -1;
    }

    objects[object_id].refcount--;
    if (objects[object_id].refcount == 0) {
        clear_object(&objects[object_id]);
    }
    return 0;
}

const vm_object_t *vm_object_get(uint32_t object_id) {
    if (object_id == 0 || object_id >= MAX_VM_OBJECTS ||
        !objects[object_id].present) {
        return 0;
    }

    return &objects[object_id];
}

int vm_object_resolve_page(uint32_t object_id, uint64_t object_offset,
                           uint32_t owner_tid, uint64_t *out_paddr) {
    if (!out_paddr) {
        return -1;
    }

    const vm_object_t *object = vm_object_get(object_id);
    if (!object || (object_offset & (PAGE_SIZE - 1)) != 0 ||
        object_offset / PAGE_SIZE >= object->page_count) {
        return -1;
    }

    if (object->type == VM_OBJECT_INITRD && object_offset < object->data_size) {
        uint64_t file_offset = object->file_offset + object_offset;
        return page_cache_get_initrd_page(object->file_index, file_offset, out_paddr);
    }

    if (object->type == VM_OBJECT_ANON ||
        (object->type == VM_OBJECT_INITRD && object_offset >= object->size)) {
        frame_t *frame = frame_alloc(owner_tid, FRAME_FLAG_USER);
        if (!frame) {
            return -1;
        }
        *out_paddr = frame->paddr;
        return 0;
    }

    return -1;
}
