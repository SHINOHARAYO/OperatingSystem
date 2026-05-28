#include "elf.h"
#include "pmm.h"
#include "frame.h"
#include "vm_object.h"
#include "vmm.h"
#include "mmu.h"
#include "log.h"

#define PAGE_SIZE 4096

uint64_t elf_load(const uint8_t *elf_data, uint64_t elf_size, uint64_t *pgd, uint16_t asid, vm_space_t *vm, uint32_t file_cap) {
    if (!elf_data || elf_size < sizeof(Elf64_Ehdr) || !pgd || !vm) {
        LOG_FAIL("ELF: Invalid arguments.");
        return 0;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        LOG_FAIL("ELF: Bad magic number.");
        return 0;
    }
    if (ehdr->e_ident[4] != ELFCLASS64) {
        LOG_FAIL("ELF: Not a 64-bit ELF.");
        return 0;
    }
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        LOG_FAIL("ELF: Not little-endian.");
        return 0;
    }
    if (ehdr->e_machine != EM_AARCH64) {
        LOG_FAIL("ELF: Not AArch64.");
        return 0;
    }

    LOG_DEBUG_HEX("ELF: Entry point:    ", ehdr->e_entry);
    LOG_DEBUG_HEX("ELF: Program headers: ", ehdr->e_phnum);

    if (ehdr->e_phentsize < sizeof(Elf64_Phdr) || ehdr->e_phoff > elf_size) {
        LOG_FAIL("ELF: Invalid program header table.");
        return 0;
    }
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if ((uint64_t)i > (UINT64_MAX - ehdr->e_phoff) / ehdr->e_phentsize) {
            LOG_FAIL("ELF: Program header offset overflow.");
            return 0;
        }

        uint64_t ph_offset = ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize;
        if (ph_offset > elf_size - sizeof(Elf64_Phdr)) {
            LOG_FAIL("ELF: Program header out of bounds.");
            return 0;
        }

        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(elf_data + ph_offset);
        if (phdr->p_type != PT_LOAD) continue;

        LOG_DEBUG_HEX("ELF: Loading segment at vaddr: ", phdr->p_vaddr);
        LOG_DEBUG_HEX("ELF:   filesz: ", phdr->p_filesz);
        LOG_DEBUG_HEX("ELF:   memsz:  ", phdr->p_memsz);

        if (phdr->p_memsz < phdr->p_filesz) {
            LOG_FAIL("ELF: Segment memsz smaller than filesz.");
            return 0;
        }

        if (phdr->p_memsz == 0) {
            continue;
        }

        if (phdr->p_offset > elf_size || phdr->p_filesz > elf_size - phdr->p_offset) {
            LOG_FAIL("ELF: Segment file range out of bounds.");
            return 0;
        }

        if (phdr->p_vaddr > UINT64_MAX - phdr->p_memsz) {
            LOG_FAIL("ELF: Segment virtual range overflow.");
            return 0;
        }

        uint64_t segment_end = phdr->p_vaddr + phdr->p_memsz;
        if (segment_end > UINT64_MAX - (PAGE_SIZE - 1)) {
            LOG_FAIL("ELF: Segment page alignment overflow.");
            return 0;
        }
        uint64_t seg_start = phdr->p_vaddr & ~(PAGE_SIZE - 1);          
        uint64_t seg_end   = (segment_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t num_pages = (seg_end - seg_start) / PAGE_SIZE;

        uint64_t vma_flags = VMA_USER | VMA_ELF;
        if (phdr->p_flags & PF_R) vma_flags |= VMA_READ;
        if (phdr->p_flags & PF_W) vma_flags |= VMA_WRITE;
        if (phdr->p_flags & PF_X) vma_flags |= VMA_EXEC;

        uint64_t page_delta = phdr->p_vaddr - seg_start;
        uint64_t file_offset = phdr->p_offset >= page_delta ? phdr->p_offset - page_delta : phdr->p_offset;
        uint64_t file_size = phdr->p_filesz + page_delta;
        if (file_size > phdr->p_memsz + page_delta) {
            file_size = phdr->p_memsz + page_delta;
        }

        if (file_cap != 0 && !(phdr->p_flags & PF_W)) {
            uint32_t object_id = 0;
            if (vm_object_create_initrd(0, file_cap - 1, file_offset, file_size,
                                        seg_end - seg_start,
                                        vma_flags, &object_id) < 0) {
                LOG_FAIL("ELF: Lazy segment VM object creation failed.");
                return 0;
            }
            if (vma_add_ex(vm, seg_start, seg_end,
                           vma_flags | VMA_FILE | VMA_LAZY,
                           0, file_size, file_cap,
                           VMA_BACKING_FILE, object_id, 0, num_pages) < 0) {
                vm_object_unref(object_id);
                LOG_FAIL("ELF: Lazy segment VMA registration failed.");
                return 0;
            }
            continue;
        }

        if (vma_add(vm, seg_start, seg_end, vma_flags,
                    phdr->p_offset, phdr->p_filesz, 0) < 0) {
            LOG_FAIL("ELF: Segment VMA registration failed.");
            return 0;
        }
        uint64_t vmm_flags = VMM_FLAG_USER_CODE | VMM_FLAG_OWNED;
        if (phdr->p_flags & PF_W) {
            vmm_flags = VMM_FLAG_USER_RW | VMM_FLAG_OWNED;
        }
        for (uint64_t p = 0; p < num_pages; p++) {
            uint64_t va = seg_start + p * PAGE_SIZE;
            frame_t *frame = frame_alloc(0, FRAME_FLAG_USER);
            if (!frame) {
                LOG_FAIL("ELF: Out of physical memory!");
                return 0;
            }

            // Zero the entire page first (handles .bss and padding)
            uint8_t *page_ptr = (uint8_t *)frame->paddr;
            for (int b = 0; b < PAGE_SIZE; b++) {
                page_ptr[b] = 0;
            }
            // The segment's file data spans [p_vaddr, p_vaddr + p_filesz)
            uint64_t copy_start_va = va;
            if (copy_start_va < phdr->p_vaddr) {
                copy_start_va = phdr->p_vaddr;
            }
            uint64_t copy_end_va = va + PAGE_SIZE;
            if (copy_end_va > phdr->p_vaddr + phdr->p_filesz) {
                copy_end_va = phdr->p_vaddr + phdr->p_filesz;
            }

            if (copy_start_va < copy_end_va) {
                uint64_t file_offset = phdr->p_offset + (copy_start_va - phdr->p_vaddr);
                uint64_t page_offset = copy_start_va - va;
                uint64_t copy_len    = copy_end_va - copy_start_va;

                const uint8_t *src = elf_data + file_offset;
                uint8_t *dst = page_ptr + page_offset;
                for (uint64_t b = 0; b < copy_len; b++) {
                    dst[b] = src[b];
                }
            }

            if (vmm_map_page_asid(pgd, asid, va, frame->paddr, vmm_flags) < 0) {
                LOG_FAIL("ELF: Failed to map segment page.");
                frame_unref(frame->paddr);
                return 0;
            }
        }
    }

    return ehdr->e_entry;
}
