#include "mmu.h"
#include "uart.h"
#include "log.h"

// TTBR0 uses a 39-bit VA space, so 512 L1 entries cover 512 GiB.
// This bootstrap map is identity mapped: Virt == Phys.

uint64_t l1_table[512] __attribute__((aligned(PAGE_SIZE)));
uint64_t high_l1_table[512] __attribute__((aligned(PAGE_SIZE)));

void mmu_init(void) {
    volatile uint64_t *l1 = l1_table;
    volatile uint64_t *high_l1 = high_l1_table;

    LOG_DEBUG("MMU: Configuring MAIR_EL1...");
    // MAIR_EL1 index 0: Device-nGnRnE (0x00)
    // MAIR_EL1 index 1: Normal memory, Inner/Outer Write-Back Non-Transient (0xFF)
    uint64_t mair = (0xFFULL << (8 * MT_NORMAL)) | (0x00ULL << (8 * MT_DEVICE_nGnRnE));
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");

    LOG_DEBUG("MMU: Building initial Page Tables.");
    for (int i = 0; i < 512; i++) {
        l1[i] = 0;
        high_l1[i] = 0;
    }

    // Map 0x0000_0000 to 0x3FFF_FFFF (1 GiB) as Device Memory.
    // QEMU virt places MMIO such as UART/GIC in this range.
    l1[0] = 0x00000000 | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_ATTRINDX(MT_DEVICE_nGnRnE);

    // Map the rest of the 39-bit lower VA space as normal memory.
    // QEMU virt DRAM starts at 0x4000_0000 and grows upward as -m increases.
    for (uint64_t i = 1; i < 512; i++) {
        l1[i] = (i << 30) | PTE_VALID | PTE_BLOCK | PTE_AF |
                PTE_ATTRINDX(MT_NORMAL) | PTE_SH_INNER;
    }
    __asm__ volatile("dsb ishst" : : : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)l1_table) : "memory");
    __asm__ volatile("msr ttbr1_el1, %0" : : "r"((uint64_t)high_l1_table) : "memory");

    LOG_DEBUG("MMU: Configuring TCR_EL1...");
    // TCR_EL1 setup for 4KB pages, 39-bit virtual address (2 translation regimes)
    // TTBR0_EL1:
    // T0SZ = 25 (64 - 39 = 25)
    // IRGN0 = 0b01 (Normal memory, Inner Write-Back)
    // ORGN0 = 0b01 (Normal memory, Outer Write-Back)
    // SH0 = 0b11 (Inner Shareable)
    // TTBR1_EL1:
    // T1SZ = 25 (64 - 39 = 25)
    // IRGN1 = 0b01 (Normal memory, Inner Write-Back)
    // ORGN1 = 0b01 (Normal memory, Outer Write-Back)
    // SH1 = 0b11 (Inner Shareable)
    // TG1 = 0b10 (4KB)
    // IPS = 0b010 (40-bit physical addresses)
    
    uint64_t tcr = (25ULL << 0)   | // T0SZ
                   (1ULL << 8)    | // IRGN0
                   (1ULL << 10)   | // ORGN0
                   (3ULL << 12)   | // SH0
                   (25ULL << 16)  | // T1SZ
                   (1ULL << 24)   | // IRGN1
                   (1ULL << 26)   | // ORGN1
                   (3ULL << 28)   | // SH1
                   (2ULL << 30)   | // TG1 (0b10 for 4KB in TTBR1)
                   (2ULL << 32)   ; // IPS (40-bit PA)

    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("tlbi vmalle1" : : : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");

    LOG_DEBUG("MMU: Enabling SCTLR_EL1.M...");
    
    // Read SCTLR_EL1, enable MMU (M bit = 0), instruction cache (I bit = 12), data cache (C bit = 2)
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr) : : "memory");
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);
    
    // Ensure all previous memory writes (page tables) are visible before enabling MMU
    __asm__ volatile("dsb ish" : : : "memory");
    __asm__ volatile("isb" : : : "memory");
    __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
    __asm__ volatile("isb" : : : "memory");

    LOG_OK("MMU: Enabled Successfully.");
}
