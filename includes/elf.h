#pragma once

#include <stdint.h>
#include "vma.h"

#define ELF_MAGIC      0x464C457F

#define ELFCLASS64     2
#define ELFDATA2LSB    1
#define EM_AARCH64     0xB7
#define ET_EXEC        2

#define PT_LOAD        1

#define PF_X           0x1
#define PF_W           0x2
#define PF_R           0x4

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

// The caller must have already set tcb->pgd to a valid address space.
uint64_t elf_load(const uint8_t *elf_data, uint64_t elf_size, uint64_t *pgd, uint16_t asid, vm_space_t *vm, uint32_t file_cap);
