#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t value;
} spinlock_t;

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->value, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("yield");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}
