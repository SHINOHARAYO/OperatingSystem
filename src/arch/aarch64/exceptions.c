#include "exceptions.h"
#include "uart.h"
#include "gic.h"
#include "timer.h"
#include "log.h"
#include "sched.h"
#include "ipc.h"
#include "memcap.h"
#include "vfs.h"
#include "pmm.h"
#include "kmalloc.h"
#include "smp.h"

#define SYS_YIELD 1
#define SYS_SEND  2
#define SYS_RECV  3
#define SYS_MMAP  4
#define SYS_SLEEP 5
#define SYS_SPAWN 6
#define SYS_AWAIT_IRQ 7
#define SYS_EXIT      8
#define SYS_UPTIME    9
#define SYS_PS       10
#define SYS_KILL     11
#define SYS_AWAIT_IRQ_TIMEOUT 12
#define SYS_MEM      13
#define SYS_WAIT     14
#define SYS_POLL     15
#define SYS_SPAWN_FILE 16
#define SYS_RESERVED_17 17
#define SYS_FILE_STAT    18
#define SYS_CAP_SEND     19
#define SYS_CAP_RECV     20
#define SYS_SPAWN_EXEC   24
#define SYS_VMMAP        25
#define SYS_MEM_EXPORT   26
#define SYS_MEM_SHARE    27
#define SYS_MEM_TRANSFER 28
#define SYS_MUNMAP       29
#define SYS_MEM_LEND     30
#define SYS_MEM_REVOKE   31
#define SYS_FORK         32
#define SYS_FILE_MMAP    33
#define SYS_TASK_CAPACITY 34
#define SYS_IPC_CALL     35
#define SYS_IPC_RECV     36
#define SYS_IPC_REPLY    37
#define SYS_CAPSTAT      38
#define SYS_UPTIME_MS    39
#define SYS_UPTIME_NS    40
#define SYS_DEBUG_INFO   41
#define SYS_VFS_BIND     42
#define SYS_VFS_CALL     43
#define SYS_VFS_REPLY    44
#define SYS_VFS_INJECT   45
#define SYS_VFS_RECV     46
#define SYS_VFS_EXEC_CREATE 47
#define SYS_DMA_EXPORT   48
#define SYS_DMA_PADDR    49
#define SYS_DMA_RELEASE  50

#define EC_IABORT_LOWER_EL 0x20
#define EC_PC_ALIGN_FAULT  0x22
#define EC_DABORT_LOWER_EL 0x24
#define EC_SP_ALIGN_FAULT  0x26

extern void vector_table_el1(void);

void exceptions_init(void) {
    __asm__ volatile("msr vbar_el1, %0" : : "r"((uint64_t)&vector_table_el1));
    __asm__ volatile("msr daifclr, #2");
}

void handle_sync(uint64_t *regs) {
    uint64_t esr, elr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    uint32_t ec = (esr >> 26) & 0x3F;
    if (ec == 0x15) {
        uint64_t svc_imm = esr & 0xFFFF;
        uint64_t syscall_num = svc_imm ? svc_imm : regs[8];
        uint64_t arg0 = regs[0];
        uint64_t arg1 = regs[1];
        uint64_t arg2 = regs[2];
        
        if (syscall_num == SYS_YIELD) {
            sched_yield_syscall();
        } else if (syscall_num == SYS_SEND) {
            regs[0] = (uint64_t)-1;
        } else if (syscall_num == SYS_RECV) {
            regs[0] = (uint64_t)-1;
        } else if (syscall_num == SYS_MMAP) {
            sched_mmap_syscall(regs, arg0);
        } else if (syscall_num == SYS_SLEEP) {
            sched_sleep_syscall(regs, arg0);
        } else if (syscall_num == SYS_SPAWN) {
            sched_spawn_syscall(regs, arg0, arg1, arg2);
        } else if (syscall_num == SYS_AWAIT_IRQ) {
            sched_await_irq_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_EXIT) {
            sched_exit_syscall(regs, (int)arg0);
        } else if (syscall_num == SYS_UPTIME) {
            regs[0] = timer_get_uptime_seconds();
        } else if (syscall_num == SYS_PS) {
            sched_ps_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_KILL) {
            sched_kill_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_AWAIT_IRQ_TIMEOUT) {
            sched_await_irq_timeout_syscall(regs, (uint32_t)arg0, arg1);
        } else if (syscall_num == SYS_MEM) {
            regs[0] = pmm_get_total_memory();
            regs[1] = pmm_get_free_memory();
            regs[2] = kmalloc_get_size();
            regs[3] = kmalloc_get_used();
            regs[4] = kmalloc_get_mapped();
        } else if (syscall_num == SYS_WAIT) {
            sched_wait_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_POLL) {
            sched_poll_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_SPAWN_FILE) {
            sched_spawn_file_syscall(regs, arg0, (uint8_t)arg1);
        } else if (syscall_num == SYS_RESERVED_17) {
            regs[0] = (uint64_t)-1;
        } else if (syscall_num == SYS_FILE_STAT) {
            sched_file_stat_syscall(regs, (uint32_t)arg0, arg1, arg2);
        } else if (syscall_num == SYS_CAP_SEND) {
            regs[0] = (uint64_t)-1;
        } else if (syscall_num == SYS_CAP_RECV) {
            regs[0] = (uint64_t)-1;
        } else if (syscall_num == SYS_SPAWN_EXEC) {
            sched_spawn_exec_syscall(regs, (uint32_t)arg0, (uint8_t)arg1);
        } else if (syscall_num == SYS_VMMAP) {
            sched_vmmap_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_MEM_EXPORT) {
            memcap_export_syscall(regs, arg0, arg1, arg2);
        } else if (syscall_num == SYS_MEM_SHARE) {
            memcap_share_syscall(regs, (uint32_t)arg0, (uint32_t)arg1, arg2);
        } else if (syscall_num == SYS_MEM_TRANSFER) {
            memcap_transfer_syscall(regs, (uint32_t)arg0, (uint32_t)arg1, arg2);
        } else if (syscall_num == SYS_MUNMAP) {
            memcap_munmap_syscall(regs, arg0, arg1);
        } else if (syscall_num == SYS_MEM_LEND) {
            memcap_lend_syscall(regs, (uint32_t)arg0, (uint32_t)arg1, arg2);
        } else if (syscall_num == SYS_MEM_REVOKE) {
            memcap_revoke_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_FORK) {
            sched_fork_syscall(regs);
        } else if (syscall_num == SYS_FILE_MMAP) {
            sched_file_mmap_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_TASK_CAPACITY) {
            sched_task_capacity_syscall(regs);
        } else if (syscall_num == SYS_IPC_CALL) {
            ipc_call_syscall(regs);
        } else if (syscall_num == SYS_IPC_RECV) {
            ipc_recv_syscall(regs);
        } else if (syscall_num == SYS_IPC_REPLY) {
            ipc_reply_syscall(regs);
        } else if (syscall_num == SYS_CAPSTAT) {
            sched_capstat_syscall(regs, (uint32_t)arg0);
        } else if (syscall_num == SYS_UPTIME_MS) {
            regs[0] = timer_get_uptime_ms();
        } else if (syscall_num == SYS_UPTIME_NS) {
            regs[0] = timer_get_uptime_ns();
        } else if (syscall_num == SYS_DEBUG_INFO) {
            sched_debug_info_syscall(regs);
        } else if (syscall_num == SYS_VFS_BIND) {
            vfs_bind_syscall(regs, (uint32_t)arg0, (uint32_t)arg1);
        } else if (syscall_num == SYS_VFS_CALL) {
            vfs_call_syscall(regs);
        } else if (syscall_num == SYS_VFS_REPLY) {
            vfs_reply_syscall(regs);
        } else if (syscall_num == SYS_VFS_INJECT) {
            vfs_inject_syscall(regs, (uint32_t)arg0, arg1, arg2);
        } else if (syscall_num == SYS_VFS_RECV) {
            vfs_recv_syscall(regs);
        } else if (syscall_num == SYS_VFS_EXEC_CREATE) {
            sched_vfs_exec_create_syscall(regs, (uint32_t)arg0, arg1, arg2,
                                          (uint32_t)regs[3],
                                          (uint32_t)regs[4]);
        } else if (syscall_num == SYS_DMA_EXPORT) {
            sched_dma_export_syscall(regs, arg0, arg1, arg2);
        } else if (syscall_num == SYS_DMA_PADDR) {
            sched_dma_paddr_syscall(regs, (uint32_t)arg0, arg1);
        } else if (syscall_num == SYS_DMA_RELEASE) {
            sched_dma_release_syscall(regs, (uint32_t)arg0);
        } else {
            uart_puts("Unknown Syscall: ");
            uart_hex(syscall_num);
            uart_puts("\n");
            regs[0] = -1;
        }
        return;
    }

    if (sched_current_is_user() &&
        (ec == EC_IABORT_LOWER_EL || ec == EC_DABORT_LOWER_EL ||
         ec == EC_PC_ALIGN_FAULT || ec == EC_SP_ALIGN_FAULT || ec == 0)) {
        sched_fault_current_task(esr, elr, far);
        return;
    }

    uart_puts("\n*** SYNCHRONOUS EXCEPTION ***\n");
    uart_puts("ESR_EL1: "); uart_hex(esr); uart_puts("\n");
    uart_puts("ELR_EL1: "); uart_hex(elr); uart_puts("\n");
    uart_puts("FAR_EL1: "); uart_hex(far); uart_puts("\n");
    uart_puts("x0: "); uart_hex(regs[0]); uart_puts("\n");
    uart_puts("x8: "); uart_hex(regs[8]); uart_puts("\n");
    uart_puts("x30: "); uart_hex(regs[30]); uart_puts("\n");
    uint64_t sp_el0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    uart_puts("SP_EL0: "); uart_hex(sp_el0); uart_puts("\n");
    
    while(1) { __asm__ volatile("wfi"); }
}

void handle_irq(void) {
    uint32_t int_id = gic_acknowledge_interrupt();
    int timer_tick = 0;
    int should_reschedule = 0;
    int reschedule_ipi = 0;
    int tlb_shootdown_ipi = 0;

    if (int_id == 1) {
        reschedule_ipi = 1;
    } else if (int_id == 2) {
        tlb_shootdown_ipi = 1;
    } else if (int_id == 30) {
        timer_handle_interrupt();
        timer_tick = 1;
    } else if (int_id > 31 && int_id < 1020) {
        extern void sched_wake_irq(uint32_t irq_num);
        sched_wake_irq(int_id);
        should_reschedule = 1;
    } else if (int_id < 1020) {
        LOG_INFO_HEX("Unhandled IRQ ID: ", int_id);
    }

    // Must always signal End Of Interrupt, even if unhandled, 
    // to prevent the GIC from blocking lower priority interrupts forever.
    if (int_id < 1020) { 
        gic_end_of_interrupt(int_id);
    }

    if (tlb_shootdown_ipi) {
        smp_handle_tlb_shootdown_ipi();
    } else if (reschedule_ipi) {
        sched_handle_reschedule_ipi();
    } else if (timer_tick) {
        sched_tick();
    } else if (should_reschedule) {
        sched_reschedule();
    }
}

void handle_fiq(void) {
    uart_puts("\n*** FIQ EXCEPTION ***\n");
    while(1) { __asm__ volatile("wfi"); }
}

void handle_serror(void) {
    uart_puts("\n*** SERROR EXCEPTION ***\n");
    while(1) { __asm__ volatile("wfi"); }
}
