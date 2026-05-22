#pragma once

#include <stdint.h>

#define VFS_ID_FS 1
#define VFS_MAX_INJECT_PAGES 64

typedef struct tcb_t tcb_t;

void vfs_bind_syscall(uint64_t *regs, uint32_t vfs_id, uint32_t endpoint_cap);
void vfs_call_syscall(uint64_t *regs);
void vfs_recv_syscall(uint64_t *regs);
void vfs_reply_syscall(uint64_t *regs);
void vfs_inject_syscall(uint64_t *regs, uint32_t client_tid,
                        uint64_t client_va, uint64_t page_count);
void vfs_clear_task_state(tcb_t *task);
void vfs_task_died(uint32_t tid);
