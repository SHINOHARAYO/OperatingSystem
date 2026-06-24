#pragma once

#include <stdint.h>
#include <stddef.h>

static inline void arch_sync_icache(const void *addr, uint64_t size) {
    if (!addr || size == 0) {
        return;
    }

    uint64_t start = (uint64_t)addr & ~63ULL;
    uint64_t end = ((uint64_t)addr + size + 63ULL) & ~63ULL;

    for (uint64_t p = start; p < end; p += 64) {
        __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" : : : "memory");
    (void)end;
    __asm__ volatile("ic iallu" : : : "memory");
    __asm__ volatile("dsb ish\nisb" : : : "memory");
}
