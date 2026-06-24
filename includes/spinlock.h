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

static inline int spin_trylock(spinlock_t *lock) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags;
    __asm__ volatile("mrs %0, daif\n"
                     "msr daifset, #2"
                     : "=r"(flags)
                     :
                     : "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    __asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}
