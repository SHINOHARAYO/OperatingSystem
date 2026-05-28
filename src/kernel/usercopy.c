#include "usercopy.h"
#include "mmu.h"
#include "sched.h"
#include "vmm.h"

static int translate_user_byte(uint64_t user_va, uint8_t **kernel_ptr, int write) {
    tcb_t *task = sched_current_task();
    if (!task || !task->pgd || !kernel_ptr) {
        return -1;
    }

    if (sched_resolve_user_page(user_va, write) < 0) {
        return -1;
    }

    uint64_t pa = 0;
    uint64_t entry = 0;
    if (vmm_query_page(task->pgd, user_va, &pa, &entry) < 0) {
        return -1;
    }
    if ((entry & PTE_USER) == 0) {
        return -1;
    }

    *kernel_ptr = (uint8_t *)pa;
    return 0;
}

static int validate_user_range(uint64_t user_va, size_t len, int write) {
    if (len == 0) {
        return 0;
    }
    if (user_va == 0 || user_va > UINT64_MAX - (uint64_t)(len - 1)) {
        return -1;
    }

    size_t checked = 0;
    while (checked < len) {
        uint8_t *unused = 0;
        uint64_t va = user_va + checked;
        if (translate_user_byte(va, &unused, write) < 0) {
            return -1;
        }

        size_t page_remaining = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
        size_t remaining = len - checked;
        checked += page_remaining < remaining ? page_remaining : remaining;
    }

    return 0;
}

int user_range_readable(uint64_t user_src, size_t len) {
    return validate_user_range(user_src, len, 0);
}

int copy_from_user(void *dst, uint64_t user_src, size_t len) {
    if (!dst && len != 0) {
        return -1;
    }
    if (validate_user_range(user_src, len, 0) < 0) {
        return -1;
    }

    uint8_t *out = (uint8_t *)dst;
    size_t copied = 0;
    while (copied < len) {
        uint64_t va = user_src + copied;
        uint8_t *src = 0;
        if (translate_user_byte(va, &src, 0) < 0) {
            return -1;
        }

        size_t page_remaining = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
        size_t remaining = len - copied;
        size_t chunk = page_remaining < remaining ? page_remaining : remaining;

        for (size_t i = 0; i < chunk; i++) {
            out[copied + i] = src[i];
        }
        copied += chunk;
    }

    return 0;
}

int copy_to_user(uint64_t user_dst, const void *src, size_t len) {
    if (!src && len != 0) {
        return -1;
    }
    if (validate_user_range(user_dst, len, 1) < 0) {
        return -1;
    }

    const uint8_t *in = (const uint8_t *)src;
    size_t copied = 0;
    while (copied < len) {
        uint64_t va = user_dst + copied;
        uint8_t *dst = 0;
        if (translate_user_byte(va, &dst, 1) < 0) {
            return -1;
        }

        size_t page_remaining = PAGE_SIZE - (size_t)(va & (PAGE_SIZE - 1));
        size_t remaining = len - copied;
        size_t chunk = page_remaining < remaining ? page_remaining : remaining;

        for (size_t i = 0; i < chunk; i++) {
            dst[i] = in[copied + i];
        }
        copied += chunk;
    }

    return 0;
}

int copy_string_from_user(char *dst, size_t dst_size, uint64_t user_src) {
    if (!dst || dst_size == 0 || user_src == 0) {
        return -1;
    }

    for (size_t i = 0; i < dst_size - 1; i++) {
        if ((uint64_t)i > UINT64_MAX - user_src) {
            dst[0] = '\0';
            return -1;
        }
        if (copy_from_user(&dst[i], user_src + i, 1) < 0) {
            dst[0] = '\0';
            return -1;
        }
        if (dst[i] == '\0') {
            return (int)i;
        }
    }

    dst[dst_size - 1] = '\0';
    return -1;
}
