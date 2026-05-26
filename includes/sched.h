#pragma once

#include <stdint.h>
#include <stddef.h>
#include "orange_cat.h"
#include "vma.h"

#define TASK_STACK_SIZE 4096
#define CAP_SELF 1
#define CAP_NS 2
#define USER_STACK_TOP 0x90001000ULL
#define USER_STACK_BASE (USER_STACK_TOP - TASK_STACK_SIZE)
#define USER_STACK_GUARD_BASE (USER_STACK_BASE - TASK_STACK_SIZE)
#define USER_SHARED_BASE 0xC0000000ULL
#define USER_BOOT_INITRD_BASE 0xD0000000ULL
#define MAX_VFS_ROUTES 8

#define IPC_INLINE_BYTES 128
#define IPC_INLINE_WORDS 16
#define IPC_REPLY_INLINE_WORDS 15
#define IPC_FLAG_MEM (1ULL << 0)
#define IPC_FLAG_CAP (1ULL << 1)
#define IPC_MEM_MODE_MASK     (0xFULL << 8)
#define IPC_MEM_MODE_SHARE    (1ULL << 8)
#define IPC_MEM_MODE_TRANSFER (2ULL << 8)
#define IPC_MEM_MODE_LEND     (3ULL << 8)

#define MEM_RIGHT_READ     (1ULL << 0)
#define MEM_RIGHT_WRITE    (1ULL << 1)
#define MEM_RIGHT_SHARE    (1ULL << 2)
#define MEM_RIGHT_TRANSFER (1ULL << 3)
#define MEM_RIGHT_LEND     (1ULL << 4)

typedef enum {
    TASK_STATE_FREE,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED_ON_IPC_CALL,
    TASK_STATE_BLOCKED_ON_IPC_RECV,
    TASK_STATE_BLOCKED_ON_IPC_REPLY,
    TASK_STATE_SLEEPING,
    TASK_STATE_BLOCKED_ON_IRQ,
    TASK_STATE_BLOCKED_ON_WAIT,
    TASK_STATE_BLOCKED_ON_VFS_CALL,
    TASK_STATE_BLOCKED_ON_VFS_RECV
} task_state_t;

typedef enum {
    TASK_TERM_EXITED = 1,
    TASK_TERM_KILLED = 2,
    TASK_TERM_FAULTED = 3
} task_termination_reason_t;

// Must perfectly align with the assembly `save_all` macro
typedef struct tcb_t {
    uint64_t registers[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    
    uint32_t tid;
    task_state_t state;
    
    uint8_t priority;
    uint8_t base_priority;
    uint32_t quantum;
    uint32_t ticks_remaining;
    uint32_t wait_time;
    uint32_t sleep_ticks_remaining;
    uint8_t sched_queued;
    uint8_t timer_queued;
    uint16_t sched_padding;
    uint64_t wake_tick;
    struct tcb_t *sched_next;
    struct tcb_t *sched_prev;
    struct tcb_t *timer_next;
    struct tcb_t *timer_prev;

    uint32_t ipc_target_tid;
    uint32_t ipc_received_sender_tid;
    uint64_t ipc_msg_flags;
    uint64_t ipc_msg_len;
    uint64_t ipc_msg_payload[IPC_INLINE_WORDS];
    struct tcb_t *ipc_next;
    struct tcb_t *ipc_call_head;
    struct tcb_t *ipc_call_tail;

    uint32_t vfs_ids[MAX_VFS_ROUTES];
    uint32_t vfs_tids[MAX_VFS_ROUTES];
    uint32_t vfs_active_client_tid;
    uint32_t vfs_active_id;
    uint32_t vfs_reply_tid;
    uint64_t vfs_args[3];
    struct tcb_t *vfs_next;
    struct tcb_t *vfs_call_head;
    struct tcb_t *vfs_call_tail;

    uint32_t awaiting_irq;
    uint32_t parent_tid;
    uint32_t wait_target_tid;

    uint64_t *pgd;
    uint16_t asid;
    uint16_t reserved_asid_padding;
    uint64_t sp_el0;
    void *kernel_stack_base;
    void *user_stack_base;
    uint64_t user_heap_pointer;
    uint64_t user_shared_pointer;
    vm_space_t vm;
    orange_cat_table_t caps;
} tcb_t;

void sched_init(void);

int sched_create_task(void (*entry_point)(void), uint8_t priority);

int sched_create_user_task(const uint8_t *elf_data, uint64_t elf_size, uint8_t priority);
int sched_create_user_task_from_file(const uint8_t *elf_data, uint64_t elf_size, uint8_t priority, uint32_t initrd_index);

void sched_tick(void);
void sched_reschedule(void);

void sched_yield_syscall(void);
void sched_mmap_syscall(uint64_t *regs, uint64_t size);
void sched_sleep_syscall(uint64_t *regs, uint64_t ms);
int sched_spawn_syscall(uint64_t *regs, uint64_t elf_data, uint64_t elf_size, uint8_t priority);
int sched_spawn_file_syscall(uint64_t *regs, uint64_t name_ptr, uint8_t priority);
int sched_spawn_exec_syscall(uint64_t *regs, uint32_t exec_cap, uint8_t priority);
int sched_vfs_exec_create_syscall(uint64_t *regs, uint32_t client_tid,
                                  uint64_t elf_data, uint64_t elf_size,
                                  uint32_t file_index);
int sched_install_exec_cap_at(uint32_t tid, uint32_t cap, uint32_t initrd_index);
int sched_install_file_cap_at(uint32_t tid, uint32_t cap, uint32_t initrd_index);
int sched_map_boot_data(uint32_t tid, const void *data, uint64_t size, uint64_t user_va);
void sched_await_irq_syscall(uint64_t *regs, uint32_t irq_num);
void sched_await_irq_timeout_syscall(uint64_t *regs, uint32_t irq_num, uint64_t timeout_ms);
void sched_exit_syscall(uint64_t *regs, int exit_code);
void sched_ps_syscall(uint64_t *regs, uint32_t index);
void sched_task_capacity_syscall(uint64_t *regs);
void sched_capstat_syscall(uint64_t *regs, uint32_t slot);
void sched_debug_info_syscall(uint64_t *regs);
void sched_dma_export_syscall(uint64_t *regs, uint64_t user_va, uint64_t size,
                              uint64_t rights);
void sched_dma_paddr_syscall(uint64_t *regs, uint32_t dma_cap, uint64_t offset);
void sched_dma_release_syscall(uint64_t *regs, uint32_t dma_cap);
int  sched_kill_syscall(uint64_t *regs, uint32_t tid);
void sched_wait_syscall(uint64_t *regs, uint32_t tid);
void sched_poll_syscall(uint64_t *regs, uint32_t tid);
void sched_vmmap_syscall(uint64_t *regs, uint32_t index);
int sched_fork_syscall(uint64_t *regs);
int sched_file_stat_syscall(uint64_t *regs, uint32_t file_cap, uint64_t name_ptr, uint64_t name_cap);
void sched_file_mmap_syscall(uint64_t *regs, uint32_t file_cap);
int sched_resolve_task_page(tcb_t *task, uint64_t user_va, int write);
int sched_resolve_user_page(uint64_t user_va, int write);
int vm_unmap_range(tcb_t *task, uint64_t start, uint64_t size);
uint32_t sched_task_capacity(void);
tcb_t *sched_task_at(uint32_t index);
tcb_t *sched_find_task(uint32_t tid);
tcb_t *sched_current_task(void);
uint64_t *sched_task_trap_frame(tcb_t *task);
void sched_clear_ipc_state(tcb_t *task);
void sched_handoff_to_task(tcb_t *target);
void sched_make_ready(tcb_t *task);
int sched_current_is_user(void);
void sched_fault_current_task(uint64_t esr, uint64_t elr, uint64_t far);

uint64_t* sched_get_task_pgd(uint32_t tid);
uint16_t sched_get_task_asid(uint32_t tid);

extern tcb_t *current_task;
