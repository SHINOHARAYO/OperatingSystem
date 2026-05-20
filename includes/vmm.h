#pragma once

#include <stdint.h>
#include <stddef.h>

void vmm_init(void);

uint64_t* vmm_create_address_space(void);

void vmm_destroy_address_space(uint64_t* pgd);

void vmm_flush_va_asid(uint64_t va, uint16_t asid);
void vmm_flush_asid(uint16_t asid);
void vmm_flush_all(void);

int vmm_map_page(uint64_t* pgd, uint64_t va, uint64_t pa, uint64_t flags);
int vmm_map_page_asid(uint64_t* pgd, uint16_t asid, uint64_t va, uint64_t pa, uint64_t flags);

void vmm_unmap_page(uint64_t* pgd, uint64_t va);
void vmm_unmap_page_asid(uint64_t* pgd, uint16_t asid, uint64_t va);

uint64_t vmm_get_physical(uint64_t* pgd, uint64_t va);

int vmm_query_page(uint64_t* pgd, uint64_t va, uint64_t *pa, uint64_t *entry);
int vmm_update_page_flags_asid(uint64_t* pgd, uint16_t asid, uint64_t va,
                               uint64_t clear_mask, uint64_t set_mask);

#define VMM_FLAG_READWRITE     (PTE_AF | PTE_ATTRINDX(MT_NORMAL) | PTE_SH_INNER)
#define VMM_FLAG_DEVICE        (PTE_AF | PTE_ATTRINDX(MT_DEVICE_nGnRnE))
#define VMM_FLAG_USER_RW       (PTE_AF | PTE_ATTRINDX(MT_NORMAL) | PTE_SH_INNER | PTE_USER)
#define VMM_FLAG_USER_CODE     (PTE_AF | PTE_ATTRINDX(MT_NORMAL) | PTE_SH_INNER | PTE_USER)
#define VMM_FLAG_USER_DEVICE   (PTE_AF | PTE_ATTRINDX(MT_DEVICE_nGnRnE) | PTE_USER)
#define VMM_FLAG_OWNED         PTE_SW_OWNED
