#pragma once

#include <stdint.h>
#include "sched.h"

#define MAX_MEM_OBJECTS 64
#define MAX_MEM_MAPPINGS 128

void memcap_init(void);
uint32_t memcap_object_count(void);
uint32_t memcap_mapping_count(void);
void memcap_release_for_owner(uint32_t owner_tid);
void memcap_forget_mappings_for_target(uint32_t target_tid);
void memcap_forget_mappings_overlapping_target(uint32_t target_tid,
                                               uint64_t start, uint64_t size);

int memcap_export_syscall(uint64_t *regs, uint64_t addr, uint64_t size,
                          uint64_t rights);
int memcap_share_syscall(uint64_t *regs, uint32_t target_tid,
                         uint32_t mem_cap, uint64_t dst_hint);
int memcap_transfer_syscall(uint64_t *regs, uint32_t target_tid,
                            uint32_t mem_cap, uint64_t dst_hint);
int memcap_lend_syscall(uint64_t *regs, uint32_t target_tid,
                        uint32_t mem_cap, uint64_t dst_hint);
int memcap_revoke_syscall(uint64_t *regs, uint32_t mem_cap);
int memcap_munmap_syscall(uint64_t *regs, uint64_t addr, uint64_t size);

int memcap_import_ipc_memory(tcb_t *src, tcb_t *dst, uint64_t mem_cap,
                             uint64_t offset, uint64_t size,
                             uint64_t descriptor, uint64_t *out_va);

int vm_share_range(tcb_t *src, tcb_t *dst, uint64_t src_va, uint64_t dst_va,
                   uint64_t size, uint64_t rights);
int vm_transfer_range(tcb_t *src, tcb_t *dst, uint64_t src_va, uint64_t dst_va,
                      uint64_t size, uint64_t rights);
