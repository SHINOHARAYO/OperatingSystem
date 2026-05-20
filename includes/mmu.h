#pragma once

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

#define MT_DEVICE_nGnRnE 0
#define MT_NORMAL        1

#define PTE_VALID     (1ULL << 0)
#define PTE_TABLE     (1ULL << 1)
#define PTE_BLOCK     (0ULL << 1)
#define PTE_PAGE      (1ULL << 1)

#define PTE_AF        (1ULL << 10)
#define PTE_SH_INNER  (3ULL << 8)
#define PTE_USER      (1ULL << 6)     // AP[1] = 1 (EL0 Access)
#define PTE_READONLY  (1ULL << 7)     // AP[2] = 1 (read-only at the allowed ELs)
#define PTE_ATTRINDX(x) ((uint64_t)(x) << 2)

// Bits 55-58 are software-defined in AArch64 page descriptors.
#define PTE_SW_OWNED  (1ULL << 55)    // Physical page is owned by this address space
#define PTE_SW_COW    (1ULL << 56)    // Writable intent, currently mapped copy-on-write

void mmu_init(void);
