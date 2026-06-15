#include "memcap.h"
#include "orange_cat.h"
#include "frame.h"
#include "pmm.h"
#include "vmm.h"
#include "vm_object.h"
#include "mmu.h"

#define MEM_OBJECT_INVALID 0
#define MEM_RIGHT_ALL (MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE | MEM_RIGHT_TRANSFER | MEM_RIGHT_LEND)
#define MEM_MAPPING_SHARE 1
#define MEM_MAPPING_LEND  2

typedef struct {
    int present;
    uint32_t owner_tid;
    uint64_t source_va;
    uint64_t size;
    uint64_t page_count;
    uint64_t page_array_pages;
    uint64_t *pages;
    uint64_t flags;
} mem_object_t;

typedef struct {
    int present;
    uint32_t object_id;
    uint32_t owner_tid;
    uint32_t target_tid;
    uint64_t dst_va;
    uint64_t size;
    uint64_t flags;
} mem_mapping_t;

static mem_object_t mem_objects[MAX_MEM_OBJECTS];
static mem_mapping_t mem_mappings[MAX_MEM_MAPPINGS];

static void zero_bytes(void *ptr, uint64_t size) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

static int ranges_overlap(uint64_t a_start, uint64_t a_size,
                          uint64_t b_start, uint64_t b_size) {
    if (a_size == 0 || b_size == 0 ||
        a_size > UINT64_MAX - a_start ||
        b_size > UINT64_MAX - b_start) {
        return 0;
    }

    uint64_t a_end = a_start + a_size;
    uint64_t b_end = b_start + b_size;
    return a_start < b_end && b_start < a_end;
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static uint64_t mem_rights_to_ocap(uint64_t rights) {
    uint64_t ocap_rights = 0;
    if (rights & MEM_RIGHT_READ) {
        ocap_rights |= OCAP_RIGHT_READ;
    }
    if (rights & MEM_RIGHT_WRITE) {
        ocap_rights |= OCAP_RIGHT_WRITE;
    }
    if (rights & MEM_RIGHT_SHARE) {
        ocap_rights |= OCAP_RIGHT_SHARE;
    }
    if (rights & MEM_RIGHT_TRANSFER) {
        ocap_rights |= OCAP_RIGHT_TRANSFER;
    }
    if (rights & MEM_RIGHT_LEND) {
        ocap_rights |= OCAP_RIGHT_LEND;
    }
    return ocap_rights;
}

static uint64_t ocap_to_mem_rights(uint64_t ocap_rights) {
    uint64_t rights = 0;
    if (ocap_rights & OCAP_RIGHT_READ) {
        rights |= MEM_RIGHT_READ;
    }
    if (ocap_rights & OCAP_RIGHT_WRITE) {
        rights |= MEM_RIGHT_WRITE;
    }
    if (ocap_rights & OCAP_RIGHT_SHARE) {
        rights |= MEM_RIGHT_SHARE;
    }
    if (ocap_rights & OCAP_RIGHT_TRANSFER) {
        rights |= MEM_RIGHT_TRANSFER;
    }
    if (ocap_rights & OCAP_RIGHT_LEND) {
        rights |= MEM_RIGHT_LEND;
    }
    return rights;
}

static void clear_mem_object(mem_object_t *obj) {
    if (!obj) {
        return;
    }

    if (obj->pages) {
        for (uint64_t i = 0; i < obj->page_count; i++) {
            if (obj->pages[i]) {
                frame_unref(obj->pages[i]);
            }
        }
        pmm_free_contiguous_pages(obj->pages, obj->page_array_pages);
    }

    obj->present = 0;
    obj->owner_tid = 0;
    obj->source_va = 0;
    obj->size = 0;
    obj->page_count = 0;
    obj->page_array_pages = 0;
    obj->pages = 0;
    obj->flags = 0;
}

static void clear_mem_mapping(mem_mapping_t *mapping) {
    if (!mapping) {
        return;
    }

    mapping->present = 0;
    mapping->object_id = 0;
    mapping->owner_tid = 0;
    mapping->target_tid = 0;
    mapping->dst_va = 0;
    mapping->size = 0;
    mapping->flags = 0;
}

static mem_object_t *get_mem_object(uint32_t object_id) {
    if (object_id == MEM_OBJECT_INVALID || object_id >= MAX_MEM_OBJECTS ||
        !mem_objects[object_id].present) {
        return 0;
    }

    return &mem_objects[object_id];
}

static int allocate_mem_object(uint32_t owner_tid, uint64_t source_va,
                               uint64_t size) {
    if (size == 0 || (size & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    uint64_t page_count = size / PAGE_SIZE;
    uint64_t array_bytes = page_count * sizeof(uint64_t);
    uint64_t array_pages = (array_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 1; i < MAX_MEM_OBJECTS; i++) {
        if (!mem_objects[i].present) {
            uint64_t *pages = (uint64_t *)pmm_alloc_contiguous_pages(array_pages);
            if (!pages) {
                return -1;
            }
            zero_bytes(pages, array_pages * PAGE_SIZE);

            mem_objects[i].present = 1;
            mem_objects[i].owner_tid = owner_tid;
            mem_objects[i].source_va = source_va;
            mem_objects[i].size = size;
            mem_objects[i].page_count = page_count;
            mem_objects[i].page_array_pages = array_pages;
            mem_objects[i].pages = pages;
            mem_objects[i].flags = 0;
            return (int)i;
        }
    }

    return -1;
}

static int record_mem_mapping(uint32_t object_id, uint32_t owner_tid,
                              uint32_t target_tid, uint64_t dst_va,
                              uint64_t size, uint64_t flags) {
    for (uint32_t i = 1; i < MAX_MEM_MAPPINGS; i++) {
        if (!mem_mappings[i].present) {
            mem_mappings[i].present = 1;
            mem_mappings[i].object_id = object_id;
            mem_mappings[i].owner_tid = owner_tid;
            mem_mappings[i].target_tid = target_tid;
            mem_mappings[i].dst_va = dst_va;
            mem_mappings[i].size = size;
            mem_mappings[i].flags = flags;
            return 0;
        }
    }

    return -1;
}

static int is_page_range(uint64_t start, uint64_t size) {
    return size != 0 &&
           (start & (PAGE_SIZE - 1)) == 0 &&
           (size & (PAGE_SIZE - 1)) == 0 &&
           size <= UINT64_MAX - start &&
           start + size <= VMA_USER_LIMIT;
}

static int validate_mem_rights(uint64_t rights) {
    return rights != 0 && (rights & ~MEM_RIGHT_ALL) == 0 &&
           (rights & (MEM_RIGHT_READ | MEM_RIGHT_WRITE)) != 0;
}

static int validate_export_range(tcb_t *task, uint64_t start, uint64_t size,
                                 uint64_t rights) {
    if (!task || !task->pgd || !is_page_range(start, size) ||
        !validate_mem_rights(rights)) {
        return -1;
    }

    uint64_t end = start + size;
    int write = (rights & MEM_RIGHT_WRITE) ? 1 : 0;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vma_t *vma = vma_find(&task->vm, va);
        if (!vma || !(vma->flags & VMA_USER) ||
            (vma->flags & (VMA_GUARD | VMA_STACK))) {
            return -1;
        }
        if ((rights & MEM_RIGHT_READ) && !(vma->flags & VMA_READ)) {
            return -1;
        }
        if ((rights & MEM_RIGHT_WRITE) && !(vma->flags & VMA_WRITE)) {
            return -1;
        }

        if (sched_resolve_task_page(task, va, write) < 0) {
            return -1;
        }

        uint64_t pa = 0;
        uint64_t entry = 0;
        if (vmm_query_page(task->pgd, va, &pa, &entry) < 0 ||
            (entry & PTE_USER) == 0 ||
            !frame_from_paddr(pa & ~(PAGE_SIZE - 1))) {
            return -1;
        }
    }

    return 0;
}

static int pin_export_range(tcb_t *task, mem_object_t *obj, uint64_t rights) {
    if (!task || !obj || !obj->pages ||
        validate_export_range(task, obj->source_va, obj->size, rights) < 0) {
        return -1;
    }

    for (uint64_t i = 0; i < obj->page_count; i++) {
        uint64_t pa = 0;
        uint64_t entry = 0;
        uint64_t va = obj->source_va + (i * PAGE_SIZE);
        if (vmm_query_page(task->pgd, va, &pa, &entry) < 0 ||
            (entry & PTE_USER) == 0 ||
            frame_ref(pa & ~(PAGE_SIZE - 1)) < 0) {
            return -1;
        }
        obj->pages[i] = pa & ~(PAGE_SIZE - 1);
    }

    return 0;
}

static int validate_munmap_range(tcb_t *task, uint64_t start, uint64_t size) {
    if (!task || !task->pgd || !is_page_range(start, size)) {
        return -1;
    }

    uint64_t end = start + size;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vma_t *vma = vma_find(&task->vm, va);
        if (!vma || !(vma->flags & VMA_USER) ||
            (vma->flags & (VMA_GUARD | VMA_STACK | VMA_ELF)) ||
            !(vma->flags & VMA_MMAP)) {
            return -1;
        }
    }

    return 0;
}

static int choose_import_destination(tcb_t *target, uint64_t size,
                                     uint64_t dst_hint, uint64_t *out_va) {
    if (!target || !target->pgd || !out_va ||
        (size & (PAGE_SIZE - 1)) != 0 || size == 0) {
        return -1;
    }

    if (dst_hint != 0) {
        if (!is_page_range(dst_hint, size) ||
            !vma_range_is_free(&target->vm, dst_hint, dst_hint + size)) {
            return -1;
        }
        *out_va = dst_hint;
        return 0;
    }

    uint64_t va = target->user_shared_pointer;
    if (va < USER_SHARED_BASE) {
        va = USER_SHARED_BASE;
    }
    va = (va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    while (size <= UINT64_MAX - va && va + size <= VMA_USER_LIMIT) {
        if (vma_range_is_free(&target->vm, va, va + size)) {
            *out_va = va;
            target->user_shared_pointer = va + size;
            return 0;
        }
        va += PAGE_SIZE;
    }

    return -1;
}

static uint64_t mem_object_vma_flags(uint64_t rights) {
    uint64_t flags = VMA_USER | VMA_MMAP;
    if (rights & MEM_RIGHT_READ) {
        flags |= VMA_READ;
    }
    if (rights & MEM_RIGHT_WRITE) {
        flags |= VMA_WRITE;
    }
    return flags;
}

static uint64_t mem_object_pte_flags(uint64_t rights) {
    uint64_t flags = VMM_FLAG_USER_RW | VMM_FLAG_OWNED;
    if (!(rights & MEM_RIGHT_WRITE)) {
        flags |= PTE_READONLY;
    }
    return flags;
}

static int map_mem_object_range(tcb_t *target, const mem_object_t *obj,
                                uint64_t object_offset, uint64_t dst_va,
                                uint64_t size, uint64_t rights) {
    if (!target || !target->pgd || !obj || !obj->pages ||
        !is_page_range(dst_va, size) ||
        (object_offset & (PAGE_SIZE - 1)) != 0 ||
        !validate_mem_rights(rights) ||
        object_offset > obj->size || size > obj->size - object_offset ||
        !vma_range_is_free(&target->vm, dst_va, dst_va + size)) {
        return -1;
    }

    uint64_t page_count = size / PAGE_SIZE;
    uint64_t first_page = object_offset / PAGE_SIZE;
    uint64_t flags = mem_object_vma_flags(rights);

    if (vma_add_ex(&target->vm, dst_va, dst_va + size, flags,
                   0, 0, 0, VMA_BACKING_NONE, 0,
                   page_count, page_count) < 0) {
        return -1;
    }

    uint64_t mapped = 0;
    for (; mapped < page_count; mapped++) {
        uint64_t paddr = obj->pages[first_page + mapped];
        if (!paddr || frame_ref(paddr) < 0) {
            goto fail;
        }

        if (vmm_map_page_asid(target->pgd, target->asid,
                              dst_va + (mapped * PAGE_SIZE), paddr,
                              mem_object_pte_flags(rights)) < 0) {
            frame_unref(paddr);
            goto fail;
        }
    }

    return 0;

fail:
    for (uint64_t i = 0; i < mapped; i++) {
        vmm_unmap_page_asid(target->pgd, target->asid, dst_va + (i * PAGE_SIZE));
    }
    vma_remove(&target->vm, dst_va, dst_va + size);
    return -1;
}

static void unmap_mem_object_source_if_present(const mem_object_t *obj,
                                               uint64_t object_offset,
                                               uint64_t size) {
    if (!obj || obj->owner_tid == 0 || obj->source_va == 0 ||
        object_offset > obj->size || size > obj->size - object_offset) {
        return;
    }

    tcb_t *owner = sched_find_task(obj->owner_tid);
    if (!owner || !owner->pgd) {
        if (owner) {
            sched_task_put(owner);
        }
        return;
    }

    uint64_t start = obj->source_va + object_offset;
    uint64_t end = start + size;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vma_t *vma = vma_find(&owner->vm, va);
        if (!vma || va < vma->start || va >= vma->end ||
            !(vma->flags & VMA_USER) || (vma->flags & VMA_GUARD)) {
            sched_task_put(owner);
            return;
        }
    }

    (void)vm_unmap_range(owner, start, size);
    sched_task_put(owner);
}

static int cap_to_mem_object(tcb_t *task, uint32_t mem_cap,
                             mem_object_t **out_obj, uint32_t *out_object_id,
                             uint64_t *out_rights) {
    ocap_t cap;
    if (!task || ocap_lookup(&task->caps, mem_cap, OCAP_VMA, 0, &cap) < 0) {
        return -1;
    }

    uint32_t object_id = cap.object_id;
    mem_object_t *obj = get_mem_object(object_id);
    if (!obj || obj->owner_tid != task->tid) {
        return -1;
    }

    if (out_obj) {
        *out_obj = obj;
    }
    if (out_object_id) {
        *out_object_id = object_id;
    }
    if (out_rights) {
        *out_rights = ocap_to_mem_rights(cap.rights);
    }
    return 0;
}

static void revoke_mappings_for_object(uint32_t object_id) {
    if (object_id == MEM_OBJECT_INVALID || object_id >= MAX_MEM_OBJECTS) {
        return;
    }

    for (uint32_t i = 1; i < MAX_MEM_MAPPINGS; i++) {
        mem_mapping_t *mapping = &mem_mappings[i];
        if (!mapping->present || mapping->object_id != object_id) {
            continue;
        }

        tcb_t *target = sched_find_task(mapping->target_tid);
        if (target && target->pgd) {
            vm_unmap_range(target, mapping->dst_va, mapping->size);
        }
        if (target) {
            sched_task_put(target);
        }
        clear_mem_mapping(mapping);
    }
}

static void memcap_ocap_release(const ocap_t *cap) {
    if (!cap || cap->type != OCAP_VMA ||
        cap->object_id == MEM_OBJECT_INVALID || cap->object_id >= MAX_MEM_OBJECTS) {
        return;
    }

    if (!mem_objects[cap->object_id].present) {
        return;
    }

    revoke_mappings_for_object(cap->object_id);
    clear_mem_object(&mem_objects[cap->object_id]);
}

void memcap_init(void) {
    ocap_set_release_hook(memcap_ocap_release);

    for (uint32_t i = 0; i < MAX_MEM_OBJECTS; i++) {
        clear_mem_object(&mem_objects[i]);
    }
    for (uint32_t i = 0; i < MAX_MEM_MAPPINGS; i++) {
        clear_mem_mapping(&mem_mappings[i]);
    }
}

uint32_t memcap_object_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < MAX_MEM_OBJECTS; i++) {
        if (mem_objects[i].present) {
            count++;
        }
    }
    return count;
}

uint32_t memcap_mapping_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < MAX_MEM_MAPPINGS; i++) {
        if (mem_mappings[i].present) {
            count++;
        }
    }
    return count;
}

void memcap_release_for_owner(uint32_t owner_tid) {
    if (owner_tid == 0) {
        return;
    }

    for (uint32_t i = 1; i < MAX_MEM_OBJECTS; i++) {
        if (mem_objects[i].present && mem_objects[i].owner_tid == owner_tid) {
            revoke_mappings_for_object(i);
            clear_mem_object(&mem_objects[i]);
        }
    }
}

void memcap_forget_mappings_for_target(uint32_t target_tid) {
    if (target_tid == 0) {
        return;
    }

    for (uint32_t i = 1; i < MAX_MEM_MAPPINGS; i++) {
        if (mem_mappings[i].present && mem_mappings[i].target_tid == target_tid) {
            clear_mem_mapping(&mem_mappings[i]);
        }
    }
}

void memcap_forget_mappings_overlapping_target(uint32_t target_tid,
                                               uint64_t start, uint64_t size) {
    if (target_tid == 0) {
        return;
    }

    for (uint32_t i = 1; i < MAX_MEM_MAPPINGS; i++) {
        mem_mapping_t *mapping = &mem_mappings[i];
        if (mapping->present && mapping->target_tid == target_tid &&
            ranges_overlap(mapping->dst_va, mapping->size, start, size)) {
            clear_mem_mapping(mapping);
        }
    }
}

int memcap_export_syscall(uint64_t *regs, uint64_t addr, uint64_t size,
                          uint64_t rights) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd ||
        validate_export_range(current, addr, size, rights) < 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int object_id = allocate_mem_object(current->tid, addr, size);
    if (object_id < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    if (pin_export_range(current, &mem_objects[object_id], rights) < 0) {
        clear_mem_object(&mem_objects[object_id]);
        regs[0] = (uint64_t)-1;
        return -1;
    }

    int cap = ocap_install(&current->caps, OCAP_VMA, (uint32_t)object_id,
                           mem_rights_to_ocap(rights) | OCAP_RIGHT_REVOKE, 0);
    if (cap < 0) {
        clear_mem_object(&mem_objects[object_id]);
        regs[0] = (uint64_t)-1;
        return -1;
    }

    regs[0] = (uint64_t)cap;
    return cap;
}

int memcap_munmap_syscall(uint64_t *regs, uint64_t addr, uint64_t size) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd ||
        validate_munmap_range(current, addr, size) < 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    if (vm_unmap_range(current, addr, size) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }
    memcap_forget_mappings_overlapping_target(current->tid, addr, size);

    regs[0] = 0;
    return 0;
}

int memcap_share_syscall(uint64_t *regs, uint32_t target_tid,
                         uint32_t mem_cap, uint64_t dst_hint) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd) {
        if (regs) regs[0] = 0;
        return -1;
    }

    mem_object_t *obj = 0;
    uint32_t object_id = 0;
    uint64_t cap_rights = 0;
    if (cap_to_mem_object(current, mem_cap, &obj, &object_id, &cap_rights) < 0 ||
        !(cap_rights & MEM_RIGHT_SHARE)) {
        regs[0] = 0;
        return -1;
    }

    tcb_t *target = sched_find_task(target_tid);
    uint64_t dst_va = 0;
    if (!target || choose_import_destination(target, obj->size, dst_hint, &dst_va) < 0 ||
        map_mem_object_range(target, obj, 0, dst_va, obj->size, cap_rights) < 0) {
        regs[0] = 0;
        if (target) {
            sched_task_put(target);
        }
        return -1;
    }

    if (record_mem_mapping(object_id, current->tid, target_tid, dst_va,
                           obj->size, MEM_MAPPING_SHARE) < 0) {
        vm_unmap_range(target, dst_va, obj->size);
        regs[0] = 0;
        sched_task_put(target);
        return -1;
    }

    regs[0] = dst_va;
    sched_task_put(target);
    return 0;
}

int memcap_transfer_syscall(uint64_t *regs, uint32_t target_tid,
                            uint32_t mem_cap, uint64_t dst_hint) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd) {
        if (regs) regs[0] = 0;
        return -1;
    }

    mem_object_t *obj = 0;
    uint64_t cap_rights = 0;
    if (cap_to_mem_object(current, mem_cap, &obj, 0, &cap_rights) < 0 ||
        !(cap_rights & MEM_RIGHT_TRANSFER)) {
        regs[0] = 0;
        return -1;
    }

    tcb_t *target = sched_find_task(target_tid);
    uint64_t dst_va = 0;
    if (!target || choose_import_destination(target, obj->size, dst_hint, &dst_va) < 0 ||
        map_mem_object_range(target, obj, 0, dst_va, obj->size, cap_rights) < 0) {
        regs[0] = 0;
        if (target) {
            sched_task_put(target);
        }
        return -1;
    }

    unmap_mem_object_source_if_present(obj, 0, obj->size);
    ocap_revoke_slot(&current->caps, mem_cap);
    regs[0] = dst_va;
    sched_task_put(target);
    return 0;
}

int memcap_lend_syscall(uint64_t *regs, uint32_t target_tid,
                        uint32_t mem_cap, uint64_t dst_hint) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd) {
        if (regs) regs[0] = 0;
        return -1;
    }

    mem_object_t *obj = 0;
    uint32_t object_id = 0;
    uint64_t cap_rights = 0;
    if (cap_to_mem_object(current, mem_cap, &obj, &object_id, &cap_rights) < 0 ||
        !(cap_rights & MEM_RIGHT_LEND)) {
        regs[0] = 0;
        return -1;
    }

    tcb_t *target = sched_find_task(target_tid);
    uint64_t dst_va = 0;
    if (!target || choose_import_destination(target, obj->size, dst_hint, &dst_va) < 0 ||
        map_mem_object_range(target, obj, 0, dst_va, obj->size, cap_rights) < 0) {
        regs[0] = 0;
        if (target) {
            sched_task_put(target);
        }
        return -1;
    }

    if (record_mem_mapping(object_id, current->tid, target_tid, dst_va,
                           obj->size, MEM_MAPPING_LEND) < 0) {
        vm_unmap_range(target, dst_va, obj->size);
        regs[0] = 0;
        sched_task_put(target);
        return -1;
    }

    regs[0] = dst_va;
    sched_task_put(target);
    return 0;
}

int memcap_revoke_syscall(uint64_t *regs, uint32_t mem_cap) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || !current->pgd) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    mem_object_t *obj = 0;
    uint32_t object_id = 0;
    uint64_t cap_rights = 0;
    if (cap_to_mem_object(current, mem_cap, &obj, &object_id, &cap_rights) < 0 ||
        !(current->caps.entries[mem_cap].rights & OCAP_RIGHT_REVOKE)) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    (void)obj;
    revoke_mappings_for_object(object_id);
    regs[0] = 0;
    return 0;
}

int memcap_import_ipc_memory(tcb_t *src, tcb_t *dst, uint64_t mem_cap,
                             uint64_t offset, uint64_t size,
                             uint64_t descriptor, uint64_t *out_va) {
    if (!src || !dst || !out_va || (offset & (PAGE_SIZE - 1)) != 0 ||
        (size & (PAGE_SIZE - 1)) != 0 || size == 0) {
        return -1;
    }

    mem_object_t *obj = 0;
    uint32_t object_id = 0;
    uint64_t rights = descriptor & MEM_RIGHT_ALL;
    uint64_t mode = descriptor & IPC_MEM_MODE_MASK;
    uint64_t cap_rights = 0;
    if (cap_to_mem_object(src, (uint32_t)mem_cap, &obj, &object_id, &cap_rights) < 0 ||
        !validate_mem_rights(rights) || (rights & ~cap_rights) != 0 ||
        offset > obj->size || size > obj->size - offset) {
        return -1;
    }

    if (choose_import_destination(dst, size, 0, out_va) < 0) {
        return -1;
    }

    if (mode == IPC_MEM_MODE_SHARE) {
        if (!(cap_rights & MEM_RIGHT_SHARE) ||
            map_mem_object_range(dst, obj, offset, *out_va, size, rights) < 0) {
            return -1;
        }
        if (record_mem_mapping(object_id, src->tid, dst->tid, *out_va, size,
                               MEM_MAPPING_SHARE) < 0) {
            vm_unmap_range(dst, *out_va, size);
            return -1;
        }
    } else if (mode == IPC_MEM_MODE_TRANSFER) {
        if (!(cap_rights & MEM_RIGHT_TRANSFER) ||
            map_mem_object_range(dst, obj, offset, *out_va, size, rights) < 0) {
            return -1;
        }
        unmap_mem_object_source_if_present(obj, offset, size);
        ocap_revoke_slot(&src->caps, (uint32_t)mem_cap);
    } else if (mode == IPC_MEM_MODE_LEND) {
        if (!(cap_rights & MEM_RIGHT_LEND) ||
            map_mem_object_range(dst, obj, offset, *out_va, size, rights) < 0) {
            return -1;
        }
        if (record_mem_mapping(object_id, src->tid, dst->tid, *out_va, size,
                               MEM_MAPPING_LEND) < 0) {
            vm_unmap_range(dst, *out_va, size);
            return -1;
        }
    } else {
        return -1;
    }

    return 0;
}

static uint64_t mem_import_vma_flags(const vma_t *src_vma, uint64_t rights) {
    uint64_t flags = src_vma->flags & ~(VMA_GUARD | VMA_LAZY | VMA_GROWS_DOWN | VMA_STACK);
    flags |= VMA_USER | VMA_MMAP;

    if (rights & MEM_RIGHT_READ) {
        flags |= VMA_READ;
    } else {
        flags &= ~VMA_READ;
    }

    if (rights & MEM_RIGHT_WRITE) {
        flags |= VMA_WRITE;
    } else {
        flags &= ~VMA_WRITE;
    }

    return flags;
}

static int add_import_vmas(tcb_t *src, tcb_t *dst, uint64_t src_va,
                           uint64_t dst_va, uint64_t size, uint64_t rights) {
    uint64_t offset = 0;
    uint64_t end = src_va + size;

    while (offset < size) {
        uint64_t cur_src = src_va + offset;
        vma_t *src_vma = vma_find(&src->vm, cur_src);
        if (!src_vma || src_vma->start > cur_src || cur_src >= src_vma->end ||
            (src_vma->flags & VMA_GUARD)) {
            return -1;
        }

        uint64_t chunk_end = min_u64(src_vma->end, end);
        uint64_t chunk_size = chunk_end - cur_src;
        uint64_t chunk_pages = chunk_size / PAGE_SIZE;
        uint64_t dst_flags = mem_import_vma_flags(src_vma, rights);
        uint64_t file_offset = src_vma->file_offset + (cur_src - src_vma->start);
        uint64_t file_size = 0;
        if (src_vma->file_size > (cur_src - src_vma->start)) {
            file_size = min_u64(src_vma->file_size - (cur_src - src_vma->start), chunk_size);
        }

        if (src_vma->object_id != 0 && vm_object_ref(src_vma->object_id) < 0) {
            return -1;
        }

        if (vma_add_ex(&dst->vm, dst_va + offset, dst_va + offset + chunk_size,
                       dst_flags, file_offset, file_size, src_vma->file_cap,
                       src_vma->backing_type, src_vma->object_id,
                       chunk_pages, chunk_pages) < 0) {
            if (src_vma->object_id != 0) {
                vm_object_unref(src_vma->object_id);
            }
            return -1;
        }

        offset += chunk_size;
    }

    return 0;
}

int vm_share_range(tcb_t *src, tcb_t *dst, uint64_t src_va, uint64_t dst_va,
                   uint64_t size, uint64_t rights) {
    if (!src || !dst || !src->pgd || !dst->pgd || size == 0 ||
        (src_va & (PAGE_SIZE - 1)) != 0 ||
        (dst_va & (PAGE_SIZE - 1)) != 0 ||
        (size & (PAGE_SIZE - 1)) != 0 ||
        size > UINT64_MAX - src_va || size > UINT64_MAX - dst_va ||
        !validate_mem_rights(rights)) {
        return -1;
    }

    if (add_import_vmas(src, dst, src_va, dst_va, size, rights) < 0) {
        vma_remove(&dst->vm, dst_va, dst_va + size);
        return -1;
    }

    uint64_t mapped = 0;
    for (; mapped < size; mapped += PAGE_SIZE) {
        uint64_t pa = 0;
        uint64_t entry = 0;
        if (vmm_query_page(src->pgd, src_va + mapped, &pa, &entry) < 0 ||
            (entry & PTE_USER) == 0 ||
            frame_ref(pa & ~(PAGE_SIZE - 1)) < 0) {
            goto fail;
        }

        uint64_t map_flags = (entry & ~0x0000FFFFFFFFF000ULL) | VMM_FLAG_OWNED;
        if (!(rights & MEM_RIGHT_WRITE)) {
            map_flags |= PTE_READONLY;
        }
        if (vmm_map_page_asid(dst->pgd, dst->asid, dst_va + mapped,
                              pa & ~(PAGE_SIZE - 1), map_flags) < 0) {
            frame_unref(pa & ~(PAGE_SIZE - 1));
            goto fail;
        }
    }

    return 0;

fail:
    for (uint64_t va = dst_va; va < dst_va + mapped; va += PAGE_SIZE) {
        vmm_unmap_page_asid(dst->pgd, dst->asid, va);
    }
    vma_remove(&dst->vm, dst_va, dst_va + size);
    return -1;
}

int vm_transfer_range(tcb_t *src, tcb_t *dst, uint64_t src_va, uint64_t dst_va,
                      uint64_t size, uint64_t rights) {
    if (vm_share_range(src, dst, src_va, dst_va, size, rights) < 0) {
        return -1;
    }

    return vm_unmap_range(src, src_va, size);
}
