#include "vma.h"
#include "pmm.h"
#include "mmu.h"
#include "vm_object.h"

static int is_page_aligned(uint64_t value) {
    return (value & (PAGE_SIZE - 1)) == 0;
}

static int same_merge_class(const vma_t *a, const vma_t *b) {
    uint64_t a_span = a->end - a->start;
    uint64_t a_next_offset = a->file_offset + a_span;

    return a->flags == b->flags &&
           a->file_cap == b->file_cap &&
           a->backing_type == b->backing_type &&
           a->object_id == b->object_id &&
           a_next_offset == b->file_offset &&
           (a->flags & VMA_FILE) == (b->flags & VMA_FILE);
}

static void release_vma_object(const vma_t *vma) {
    if (vma && vma->object_id != 0) {
        vm_object_unref(vma->object_id);
    }
}

static uint64_t page_count(uint64_t start, uint64_t end) {
    return (end - start) / PAGE_SIZE;
}

static uint32_t vma_capacity_for_pages(uint32_t pages) {
    return (uint32_t)(((uint64_t)pages * PAGE_SIZE) / sizeof(vma_t));
}

static void zero_bytes(void *ptr, uint64_t len) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (uint64_t i = 0; i < len; i++) {
        bytes[i] = 0;
    }
}

static uint32_t lower_bound_end(const vm_space_t *vm, uint64_t addr) {
    uint32_t lo = 0;
    uint32_t hi = vm ? vm->count : 0;

    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2);
        if (vm->vmas[mid].end <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}

static int vma_grow(vm_space_t *vm) {
    if (!vm || !vm->vmas || vm->table_pages == 0) {
        return -1;
    }

    uint32_t new_pages = vm->table_pages * 2;
    vma_t *new_vmas = (vma_t *)pmm_alloc_contiguous_pages(new_pages);
    if (!new_vmas) {
        return -1;
    }

    zero_bytes(new_vmas, (uint64_t)new_pages * PAGE_SIZE);
    for (uint32_t i = 0; i < vm->count; i++) {
        new_vmas[i] = vm->vmas[i];
    }

    pmm_free_contiguous_pages(vm->vmas, vm->table_pages);
    vm->vmas = new_vmas;
    vm->table_pages = new_pages;
    vm->capacity = vma_capacity_for_pages(new_pages);
    vm->last_vma = -1;
    return 0;
}

static void shrink_vma_accounting(vma_t *vma, uint64_t removed_pages) {
    if (removed_pages >= vma->max_pages) {
        vma->max_pages = 0;
    } else {
        vma->max_pages -= removed_pages;
    }

    if (removed_pages >= vma->committed_pages) {
        vma->committed_pages = 0;
    } else {
        vma->committed_pages -= removed_pages;
    }
}

static void split_vma_accounting(vma_t *left, vma_t *right,
                                 uint64_t old_start, uint64_t cut_start,
                                 uint64_t cut_end, uint64_t old_end) {
    uint64_t left_pages = page_count(old_start, cut_start);
    uint64_t right_pages = page_count(cut_end, old_end);
    uint64_t removed_pages = page_count(cut_start, cut_end);
    uint64_t old_committed = left->committed_pages;

    left->max_pages = left_pages;
    right->max_pages = right_pages;

    if (old_committed >= left_pages + removed_pages) {
        left->committed_pages = left_pages;
        uint64_t right_committed = old_committed - left_pages - removed_pages;
        right->committed_pages = right_committed > right_pages ? right_pages : right_committed;
    } else if (old_committed > left_pages) {
        left->committed_pages = left_pages;
        right->committed_pages = 0;
    } else {
        left->committed_pages = old_committed;
        right->committed_pages = 0;
    }
}

int vma_init(vm_space_t *vm) {
    if (!vm) {
        return -1;
    }

    vm->vmas = (vma_t *)pmm_alloc_page();
    if (!vm->vmas) {
        vm->count = 0;
        vm->capacity = 0;
        vm->table_pages = 0;
        vm->last_vma = -1;
        return -1;
    }

    zero_bytes(vm->vmas, PAGE_SIZE);

    vm->count = 0;
    vm->table_pages = 1;
    vm->capacity = vma_capacity_for_pages(vm->table_pages);
    vm->last_vma = -1;
    return 0;
}

void vma_destroy(vm_space_t *vm) {
    if (!vm) {
        return;
    }

    if (vm->vmas) {
        for (uint32_t i = 0; i < vm->count; i++) {
            release_vma_object(&vm->vmas[i]);
        }
        pmm_free_contiguous_pages(vm->vmas, vm->table_pages ? vm->table_pages : 1);
    }

    vm->vmas = 0;
    vm->count = 0;
    vm->capacity = 0;
    vm->table_pages = 0;
    vm->last_vma = -1;
}

int vma_add(vm_space_t *vm, uint64_t start, uint64_t end, uint64_t flags,
            uint64_t file_offset, uint64_t file_size, uint32_t file_cap) {
    if (start >= end) {
        return -1;
    }

    uint64_t pages = (end - start) / PAGE_SIZE;
    uint64_t committed = (flags & (VMA_LAZY | VMA_GUARD)) ? 0 : pages;
    uint32_t backing = (flags & VMA_MMAP) ? VMA_BACKING_ANON :
                       (flags & VMA_FILE) ? VMA_BACKING_FILE : VMA_BACKING_NONE;

    return vma_add_ex(vm, start, end, flags, file_offset, file_size, file_cap,
                      backing, 0, committed, pages);
}

int vma_add_ex(vm_space_t *vm, uint64_t start, uint64_t end, uint64_t flags,
               uint64_t file_offset, uint64_t file_size, uint32_t file_cap,
               uint32_t backing_type, uint32_t object_id,
               uint64_t committed_pages, uint64_t max_pages) {
    if (!vm || !vm->vmas || start >= end || end > VMA_USER_LIMIT ||
        !is_page_aligned(start) || !is_page_aligned(end)) {
        return -1;
    }

    uint64_t pages = (end - start) / PAGE_SIZE;
    if (max_pages == 0) {
        max_pages = pages;
    }
    if (committed_pages > max_pages || pages > max_pages) {
        return -1;
    }

    uint32_t pos = lower_bound_end(vm, start);

    if (pos < vm->count && vm->vmas[pos].start < end) {
        return -1;
    }

    vma_t new_vma;
    new_vma.start = start;
    new_vma.end = end;
    new_vma.flags = flags;
    new_vma.file_offset = file_offset;
    new_vma.file_size = file_size;
    new_vma.committed_pages = committed_pages;
    new_vma.max_pages = max_pages;
    new_vma.file_cap = file_cap;
    new_vma.backing_type = backing_type;
    new_vma.object_id = object_id;
    new_vma.reserved = 0;

    if (pos > 0 && vm->vmas[pos - 1].end == new_vma.start &&
        same_merge_class(&vm->vmas[pos - 1], &new_vma)) {
        vm->vmas[pos - 1].end = new_vma.end;
        vm->vmas[pos - 1].file_size += new_vma.file_size;
        vm->vmas[pos - 1].committed_pages += new_vma.committed_pages;
        vm->vmas[pos - 1].max_pages += new_vma.max_pages;
        release_vma_object(&new_vma);
        if (pos < vm->count && vm->vmas[pos - 1].end == vm->vmas[pos].start &&
            same_merge_class(&vm->vmas[pos - 1], &vm->vmas[pos])) {
            vm->vmas[pos - 1].end = vm->vmas[pos].end;
            vm->vmas[pos - 1].file_size += vm->vmas[pos].file_size;
            vm->vmas[pos - 1].committed_pages += vm->vmas[pos].committed_pages;
            vm->vmas[pos - 1].max_pages += vm->vmas[pos].max_pages;
            release_vma_object(&vm->vmas[pos]);
            for (uint32_t i = pos; i + 1 < vm->count; i++) {
                vm->vmas[i] = vm->vmas[i + 1];
            }
            vm->count--;
        }
        vm->last_vma = (int32_t)(pos - 1);
        return 0;
    }

    if (pos < vm->count && new_vma.end == vm->vmas[pos].start &&
        same_merge_class(&new_vma, &vm->vmas[pos])) {
        vm->vmas[pos].start = new_vma.start;
        vm->vmas[pos].file_offset = new_vma.file_offset;
        vm->vmas[pos].file_size += new_vma.file_size;
        vm->vmas[pos].committed_pages += new_vma.committed_pages;
        vm->vmas[pos].max_pages += new_vma.max_pages;
        release_vma_object(&new_vma);
        if (pos > 0 && vm->vmas[pos - 1].end == vm->vmas[pos].start &&
            same_merge_class(&vm->vmas[pos - 1], &vm->vmas[pos])) {
            vm->vmas[pos - 1].end = vm->vmas[pos].end;
            vm->vmas[pos - 1].file_size += vm->vmas[pos].file_size;
            vm->vmas[pos - 1].committed_pages += vm->vmas[pos].committed_pages;
            vm->vmas[pos - 1].max_pages += vm->vmas[pos].max_pages;
            release_vma_object(&vm->vmas[pos]);
            for (uint32_t i = pos; i + 1 < vm->count; i++) {
                vm->vmas[i] = vm->vmas[i + 1];
            }
            vm->count--;
            pos--;
        }
        vm->last_vma = (int32_t)pos;
        return 0;
    }

    if (vm->count >= vm->capacity && vma_grow(vm) < 0) {
        return -1;
    }

    for (uint32_t i = vm->count; i > pos; i--) {
        vm->vmas[i] = vm->vmas[i - 1];
    }

    vm->vmas[pos] = new_vma;
    vm->count++;
    vm->last_vma = (int32_t)pos;
    return 0;
}

int vma_remove(vm_space_t *vm, uint64_t start, uint64_t end) {
    if (!vm || !vm->vmas || start >= end ||
        !is_page_aligned(start) || !is_page_aligned(end)) {
        return -1;
    }

    int removed = 0;
    for (uint32_t i = lower_bound_end(vm, start); i < vm->count;) {
        vma_t *vma = &vm->vmas[i];
        if (vma->end <= start) {
            i++;
            continue;
        }
        if (vma->start >= end) {
            break;
        }

        uint64_t old_start = vma->start;
        uint64_t old_end = vma->end;
        uint64_t cut_start = start > old_start ? start : old_start;
        uint64_t cut_end = end < old_end ? end : old_end;

        if (cut_start == old_start && cut_end == old_end) {
            release_vma_object(vma);
            for (uint32_t j = i; j + 1 < vm->count; j++) {
                vm->vmas[j] = vm->vmas[j + 1];
            }
            vm->count--;
            removed = 1;
            continue;
        }

        if (cut_start == old_start) {
            uint64_t removed_bytes = cut_end - old_start;
            uint64_t removed_pages = page_count(old_start, cut_end);
            vma->start = cut_end;
            vma->file_offset += removed_bytes;
            if (vma->file_size > removed_bytes) {
                vma->file_size -= removed_bytes;
            } else {
                vma->file_size = 0;
            }
            shrink_vma_accounting(vma, removed_pages);
            removed = 1;
            i++;
            continue;
        }

        if (cut_end == old_end) {
            uint64_t removed_bytes = old_end - cut_start;
            uint64_t removed_pages = page_count(cut_start, old_end);
            vma->end = cut_start;
            if (vma->file_size > removed_bytes) {
                vma->file_size -= removed_bytes;
            } else {
                vma->file_size = 0;
            }
            shrink_vma_accounting(vma, removed_pages);
            removed = 1;
            i++;
            continue;
        }

        if (vm->count >= vm->capacity && vma_grow(vm) < 0) {
            return -1;
        }
        vma = &vm->vmas[i];
        if (vma->object_id != 0 && vm_object_ref(vma->object_id) < 0) {
            return -1;
        }

        vma_t right = *vma;
        right.start = cut_end;
        right.file_offset += cut_end - old_start;
        if (right.file_size > cut_end - old_start) {
            right.file_size -= cut_end - old_start;
        } else {
            right.file_size = 0;
        }

        vma->end = cut_start;
        if (vma->file_size > old_end - cut_start) {
            vma->file_size -= old_end - cut_start;
        } else {
            vma->file_size = 0;
        }
        split_vma_accounting(vma, &right, old_start, cut_start, cut_end, old_end);

        for (uint32_t j = vm->count; j > i + 1; j--) {
            vm->vmas[j] = vm->vmas[j - 1];
        }
        vm->vmas[i + 1] = right;
        vm->count++;
        removed = 1;
        i += 2;
    }

    vm->last_vma = -1;
    vma_coalesce(vm);
    return removed ? 0 : -1;
}

void vma_coalesce(vm_space_t *vm) {
    if (!vm || !vm->vmas || vm->count < 2) {
        return;
    }

    for (uint32_t i = 0; i + 1 < vm->count;) {
        vma_t *left = &vm->vmas[i];
        vma_t *right = &vm->vmas[i + 1];
        if (left->end == right->start && same_merge_class(left, right)) {
            left->end = right->end;
            left->file_size += right->file_size;
            left->committed_pages += right->committed_pages;
            left->max_pages += right->max_pages;
            release_vma_object(right);
            for (uint32_t j = i + 1; j + 1 < vm->count; j++) {
                vm->vmas[j] = vm->vmas[j + 1];
            }
            vm->count--;
            continue;
        }
        i++;
    }

    vm->last_vma = -1;
}

vma_t *vma_find(vm_space_t *vm, uint64_t addr) {
    if (!vm || !vm->vmas) {
        return 0;
    }

    if (vm->last_vma >= 0 && (uint32_t)vm->last_vma < vm->count) {
        vma_t *last = &vm->vmas[vm->last_vma];
        if (addr >= last->start && addr < last->end) {
            return last;
        }
    }

    uint32_t pos = lower_bound_end(vm, addr);
    if (pos < vm->count && addr >= vm->vmas[pos].start &&
        addr < vm->vmas[pos].end) {
        vm->last_vma = (int32_t)pos;
        return &vm->vmas[pos];
    }

    return 0;
}

vma_t *vma_next(vm_space_t *vm, const vma_t *vma) {
    if (!vm || !vm->vmas || !vma ||
        vma < vm->vmas || vma >= vm->vmas + vm->count) {
        return 0;
    }

    uint32_t index = (uint32_t)(vma - vm->vmas);
    if (index + 1 >= vm->count) {
        return 0;
    }

    return &vm->vmas[index + 1];
}

int vma_range_is_free(const vm_space_t *vm, uint64_t start, uint64_t end) {
    if (!vm || !vm->vmas || start >= end || end > VMA_USER_LIMIT ||
        !is_page_aligned(start) || !is_page_aligned(end)) {
        return 0;
    }

    uint32_t pos = lower_bound_end(vm, start);
    return pos >= vm->count || vm->vmas[pos].start >= end;
}

int vma_check(vm_space_t *vm, uint64_t addr, uint64_t required_flags) {
    vma_t *vma = vma_find(vm, addr);
    if (!vma || (vma->flags & VMA_GUARD)) {
        return -1;
    }

    return (vma->flags & required_flags) == required_flags ? 0 : -1;
}

const vma_t *vma_get(const vm_space_t *vm, uint32_t index) {
    if (!vm || !vm->vmas || index >= vm->count) {
        return 0;
    }

    return &vm->vmas[index];
}
