#include "vmm.h"
#include "mmu.h"
#include "pmm.h"
#include "frame.h"
#include "uart.h"
#include "log.h"
#include "smp.h"

// Pointer to the root page table (L1 for 39-bit VA space)
extern uint64_t l1_table[512];
extern uint64_t high_l1_table[512];

#define PTE_MASK 0x0000FFFFFFFFF000ULL
#define VMM_ASID_ALL 0xFFFFU

void vmm_flush_va_asid(uint64_t va, uint16_t asid) {
    smp_tlb_shootdown_va_asid(va, asid);
}

static void vmm_flush_va_all_asids(uint64_t va) {
    smp_tlb_shootdown_va_all_asids(va);
}

void vmm_flush_asid(uint16_t asid) {
    smp_tlb_shootdown_asid(asid);
}

void vmm_flush_all(void) {
    smp_tlb_shootdown_all();
}

void vmm_init(void) {
    LOG_OK("VMM: Virtual Memory Manager Initialized.");
    // We already have an active L1 root table from mmu.c identity map.
    // We can just use it.
}

uint64_t* vmm_create_address_space(void) {
    uint64_t* pgd = (uint64_t*)pmm_alloc_page();
    if (!pgd) return NULL;
    
    // Mirror the kernel's lower-half identity mappings into this address space.
    // This allows the kernel to continue executing uninterrupted while this PGD is loaded in TTBR0_EL1.
    // Notice that these mappings do NOT have PTE_USER, so EL0 applications cannot access them.
    for (int i = 0; i < 512; i++) {
        pgd[i] = l1_table[i];
    }
    
    return pgd;
}
static inline uint64_t get_l1_index(uint64_t va) {
    return (va >> 30) & 0x1FF;
}
static inline uint64_t get_l2_index(uint64_t va) {
    return (va >> 21) & 0x1FF;
}
static inline uint64_t get_l3_index(uint64_t va) {
    return (va >> 12) & 0x1FF;
}

int vmm_map_page_asid(uint64_t* pgd, uint16_t asid, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t *root_l1 = pgd;
    if (!root_l1) {
        root_l1 = (va & (1ULL << 63)) ? high_l1_table : l1_table;
    }
    uint64_t l1_idx = get_l1_index(va);
    uint64_t l2_idx = get_l2_index(va);
    uint64_t l3_idx = get_l3_index(va);

    uint64_t *l2_table;
    uint64_t *l3_table;
    if ((root_l1[l1_idx] & PTE_VALID) == 0 || (root_l1[l1_idx] & PTE_TABLE) == 0) {
        
        int is_block = (root_l1[l1_idx] & PTE_VALID) && ((root_l1[l1_idx] & PTE_TABLE) == 0);
        uint64_t old_block_pa = root_l1[l1_idx] & PTE_MASK;
        uint64_t old_block_attrs = root_l1[l1_idx] & ~PTE_MASK;
        
        if (is_block) {
             LOG_DEBUG("VMM: Splitting L1 block into L2 table.");
        }
        
        l2_table = (uint64_t *)pmm_alloc_page();
        if (!l2_table) return -1;
        
        if (is_block) {
            // Split the 1GB block into 512 x 2MB blocks to preserve mappings!
            for (int i = 0; i < 512; i++) {
                l2_table[i] = (old_block_pa + (i * 0x200000)) | (old_block_attrs & ~PTE_VALID) | PTE_VALID;
                // Note: L2 Block entries also have PTE_TABLE bit = 0.
                l2_table[i] &= ~PTE_TABLE;
            }
        }
        
        root_l1[l1_idx] = ((uint64_t)l2_table & PTE_MASK) | PTE_VALID | PTE_TABLE;
    } else {
        l2_table = (uint64_t *)(root_l1[l1_idx] & PTE_MASK);
    }
    if ((l2_table[l2_idx] & PTE_VALID) == 0 || (l2_table[l2_idx] & PTE_TABLE) == 0) {
        l3_table = (uint64_t *)pmm_alloc_page();
        if (!l3_table) return -1;
        l2_table[l2_idx] = ((uint64_t)l3_table & PTE_MASK) | PTE_VALID | PTE_TABLE;
    } else {
        l3_table = (uint64_t *)(l2_table[l2_idx] & PTE_MASK);
    }

    if ((l3_table[l3_idx] & PTE_VALID) && (l3_table[l3_idx] & PTE_SW_OWNED)) {
        uint64_t old_page = l3_table[l3_idx] & PTE_MASK;
        if (frame_unref(old_page) < 0) {
            pmm_free_page((void *)old_page);
        }
    }
    l3_table[l3_idx] = (pa & PTE_MASK) | PTE_VALID | PTE_PAGE | flags;
    
    if (asid == VMM_ASID_ALL) {
        vmm_flush_va_all_asids(va);
    } else {
        vmm_flush_va_asid(va, asid);
    }

    return 0;
}

int vmm_map_page(uint64_t* pgd, uint64_t va, uint64_t pa, uint64_t flags) {
    return vmm_map_page_asid(pgd, VMM_ASID_ALL, va, pa, flags);
}

void vmm_unmap_page_asid(uint64_t* pgd, uint16_t asid, uint64_t va) {
    uint64_t *root_l1 = pgd;
    if (!root_l1) {
        root_l1 = (va & (1ULL << 63)) ? high_l1_table : l1_table;
    }

    uint64_t l1_idx = get_l1_index(va);
    uint64_t l2_idx = get_l2_index(va);
    uint64_t l3_idx = get_l3_index(va);

    if ((root_l1[l1_idx] & PTE_VALID) == 0 || (root_l1[l1_idx] & PTE_TABLE) == 0) return;
    uint64_t *l2_table = (uint64_t *)(root_l1[l1_idx] & PTE_MASK);

    if ((l2_table[l2_idx] & PTE_VALID) == 0 || (l2_table[l2_idx] & PTE_TABLE) == 0) return;
    uint64_t *l3_table = (uint64_t *)(l2_table[l2_idx] & PTE_MASK);

    if ((l3_table[l3_idx] & PTE_VALID) && (l3_table[l3_idx] & PTE_SW_OWNED)) {
        uint64_t phys_page = l3_table[l3_idx] & PTE_MASK;
        if (frame_unref(phys_page) < 0) {
            pmm_free_page((void *)phys_page);
        }
    }
    l3_table[l3_idx] = 0;

    if (asid == VMM_ASID_ALL) {
        vmm_flush_va_all_asids(va);
    } else {
        vmm_flush_va_asid(va, asid);
    }
}

void vmm_unmap_page(uint64_t* pgd, uint64_t va) {
    vmm_unmap_page_asid(pgd, VMM_ASID_ALL, va);
}

uint64_t vmm_get_physical(uint64_t* pgd, uint64_t va) {
    uint64_t pa = 0;
    if (vmm_query_page(pgd, va, &pa, NULL) < 0) {
        return 0;
    }
    return pa;
}

int vmm_query_page(uint64_t* pgd, uint64_t va, uint64_t *pa, uint64_t *entry) {
    uint64_t *root_l1 = pgd;
    if (!root_l1) {
        root_l1 = (va & (1ULL << 63)) ? high_l1_table : l1_table;
    }

    uint64_t l1_idx = get_l1_index(va);
    uint64_t l2_idx = get_l2_index(va);
    uint64_t l3_idx = get_l3_index(va);

    if ((root_l1[l1_idx] & PTE_VALID) == 0) return -1;
    if ((root_l1[l1_idx] & PTE_TABLE) == 0) {
        if (pa) *pa = (root_l1[l1_idx] & PTE_MASK) + (va & 0x3FFFFFFFULL);
        if (entry) *entry = root_l1[l1_idx];
        return 0;
    }

    uint64_t *l2_table = (uint64_t *)(root_l1[l1_idx] & PTE_MASK);
    if ((l2_table[l2_idx] & PTE_VALID) == 0) return -1;
    if ((l2_table[l2_idx] & PTE_TABLE) == 0) {
        if (pa) *pa = (l2_table[l2_idx] & PTE_MASK) + (va & 0x1FFFFFULL);
        if (entry) *entry = l2_table[l2_idx];
        return 0;
    }

    uint64_t *l3_table = (uint64_t *)(l2_table[l2_idx] & PTE_MASK);
    if ((l3_table[l3_idx] & PTE_VALID) == 0) return -1;

    if (pa) *pa = (l3_table[l3_idx] & PTE_MASK) + (va & 0xFFFULL);
    if (entry) *entry = l3_table[l3_idx];
    return 0;
}

int vmm_update_page_flags_asid(uint64_t* pgd, uint16_t asid, uint64_t va,
                               uint64_t clear_mask, uint64_t set_mask) {
    uint64_t *root_l1 = pgd;
    if (!root_l1) {
        root_l1 = (va & (1ULL << 63)) ? high_l1_table : l1_table;
    }

    uint64_t l1_idx = get_l1_index(va);
    uint64_t l2_idx = get_l2_index(va);
    uint64_t l3_idx = get_l3_index(va);

    if ((root_l1[l1_idx] & PTE_VALID) == 0 ||
        (root_l1[l1_idx] & PTE_TABLE) == 0) {
        return -1;
    }

    uint64_t *l2_table = (uint64_t *)(root_l1[l1_idx] & PTE_MASK);
    if ((l2_table[l2_idx] & PTE_VALID) == 0 ||
        (l2_table[l2_idx] & PTE_TABLE) == 0) {
        return -1;
    }

    uint64_t *l3_table = (uint64_t *)(l2_table[l2_idx] & PTE_MASK);
    if ((l3_table[l3_idx] & PTE_VALID) == 0) {
        return -1;
    }

    l3_table[l3_idx] = (l3_table[l3_idx] & ~clear_mask) | set_mask;
    if (asid == VMM_ASID_ALL) {
        vmm_flush_va_all_asids(va);
    } else {
        vmm_flush_va_asid(va, asid);
    }
    return 0;
}

void vmm_destroy_address_space(uint64_t* pgd) {
    if (!pgd) return;

    for (int l1_idx = 0; l1_idx < 512; l1_idx++) {
        if ((pgd[l1_idx] & PTE_VALID) && (pgd[l1_idx] & PTE_TABLE)) {
            uint64_t *l2_table = (uint64_t *)(pgd[l1_idx] & PTE_MASK);
            
            for (int l2_idx = 0; l2_idx < 512; l2_idx++) {
                if ((l2_table[l2_idx] & PTE_VALID) && (l2_table[l2_idx] & PTE_TABLE)) {
                    uint64_t *l3_table = (uint64_t *)(l2_table[l2_idx] & PTE_MASK);
                    
                    for (int l3_idx = 0; l3_idx < 512; l3_idx++) {
                        if (l3_table[l3_idx] & PTE_VALID) {
                            uint64_t phys_page = l3_table[l3_idx] & PTE_MASK;
                            if (l3_table[l3_idx] & PTE_SW_OWNED) {
                                if (frame_unref(phys_page) < 0) {
                                    pmm_free_page((void*)phys_page);
                                }
                            }
                        }
                    }
                    pmm_free_page(l3_table);
                }
            }
            pmm_free_page(l2_table);
        }
    }
    pmm_free_page(pgd);
}
