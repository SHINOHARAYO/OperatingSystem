#include "sched.h"
#include "kmalloc.h"
#include "pmm.h"
#include "frame.h"
#include "vmm.h"
#include "vm_object.h"
#include "page_cache.h"
#include "mmu.h"
#include "log.h"
#include "uart.h"
#include "elf.h"
#include "initrd.h"
#include "usercopy.h"
#include "memcap.h"
#include <stdbool.h>

extern uint64_t l1_table[512];

#define MAX_USER_SPAWN_ELF_SIZE (1024 * 1024)
#define NAME_SERVER_TID 1
#define TASK_INITIAL_CAPACITY 16
#define MAX_USER_ASIDS 65535
#define USER_STACK_MAX_SIZE (64 * 1024)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

#define EC_IABORT_LOWER_EL 0x20
#define EC_DABORT_LOWER_EL 0x24
#define ESR_DABORT_WNR     (1ULL << 6)
#define TTBR_ASID_SHIFT    48
#define TTBR_BADDR_MASK    0x0000FFFFFFFFF000ULL

static tcb_t *tasks = NULL;
static uint32_t task_capacity = 0;
static uint32_t task_table_pages = 0;

typedef struct {
    int present;
    uint32_t parent_tid;
    uint32_t tid;
    uint32_t reason;
    int exit_code;
    uint64_t esr;
    uint64_t elr;
    uint64_t far;
} termination_record_t;

static termination_record_t *termination_records = NULL;
static uint32_t termination_table_pages = 0;
static void clear_caps(tcb_t *task);
static void clear_termination_record(termination_record_t *record);
static int install_cap(tcb_t *task, uint32_t type, uint32_t object_id,
                       uint64_t rights, uint64_t flags);
static void install_standard_caps(tcb_t *task);
static int install_endpoint_cap_for_tid(tcb_t *task, uint32_t tid);

tcb_t *current_task = NULL;

static int current_task_idx = 0;
static uint32_t next_tid = 1;
static uint8_t asid_in_use[MAX_USER_ASIDS + 1];
static uint16_t next_user_asid = 1;

static uint32_t table_pages_for_size(uint64_t elem_size, uint32_t count) {
    uint64_t bytes = elem_size * count;
    return (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

static void zero_bytes(void *ptr, uint64_t size) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

void sched_clear_ipc_state(tcb_t *task) {
    if (!task) {
        return;
    }

    task->ipc_target_tid = 0;
    task->ipc_received_sender_tid = 0;
    task->ipc_msg_flags = 0;
    task->ipc_msg_len = 0;
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        task->ipc_msg_payload[i] = 0;
    }
}

static int allocate_user_asid(uint16_t *out_asid) {
    if (!out_asid) {
        return -1;
    }

    for (uint32_t searched = 0; searched < MAX_USER_ASIDS; searched++) {
        uint16_t candidate = next_user_asid;
        next_user_asid = (candidate == MAX_USER_ASIDS) ? 1 : (uint16_t)(candidate + 1);

        if (!asid_in_use[candidate]) {
            asid_in_use[candidate] = 1;
            vmm_flush_asid(candidate);
            *out_asid = candidate;
            return 0;
        }
    }

    return -1;
}

static void release_user_asid(uint16_t asid) {
    if (asid == 0) {
        return;
    }

    vmm_flush_asid(asid);
    asid_in_use[asid] = 0;
}

static void init_free_task_slot(tcb_t *task) {
    if (!task) {
        return;
    }

    task->state = TASK_STATE_FREE;
    task->tid = 0;
    task->asid = 0;
    task->reserved_asid_padding = 0;
    task->vm.vmas = NULL;
    task->vm.count = 0;
    task->vm.capacity = 0;
    task->vm.table_pages = 0;
    task->vm.last_vma = -1;
    task->user_heap_pointer = 0;
    task->user_shared_pointer = 0;
    sched_clear_ipc_state(task);
    clear_caps(task);
}

static int grow_task_tables(uint32_t min_capacity) {
    uint32_t old_capacity = task_capacity;
    uint32_t new_capacity = old_capacity ? old_capacity * 2 : TASK_INITIAL_CAPACITY;
    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }

    if (new_capacity > MAX_USER_ASIDS) {
        return -1;
    }

    uint32_t new_task_pages = table_pages_for_size(sizeof(tcb_t), new_capacity);
    uint32_t new_term_pages = table_pages_for_size(sizeof(termination_record_t), new_capacity);
    tcb_t *new_tasks = (tcb_t *)pmm_alloc_contiguous_pages(new_task_pages);
    if (!new_tasks) {
        return -1;
    }

    termination_record_t *new_records =
        (termination_record_t *)pmm_alloc_contiguous_pages(new_term_pages);
    if (!new_records) {
        pmm_free_contiguous_pages(new_tasks, new_task_pages);
        return -1;
    }

    zero_bytes(new_tasks, (uint64_t)new_task_pages * PAGE_SIZE);
    zero_bytes(new_records, (uint64_t)new_term_pages * PAGE_SIZE);

    for (uint32_t i = 0; i < old_capacity; i++) {
        copy_bytes(&new_tasks[i], &tasks[i], sizeof(tcb_t));
        copy_bytes(&new_records[i], &termination_records[i],
                   sizeof(termination_record_t));
    }
    for (uint32_t i = old_capacity; i < new_capacity; i++) {
        init_free_task_slot(&new_tasks[i]);
        clear_termination_record(&new_records[i]);
    }

    tcb_t *old_tasks = tasks;
    termination_record_t *old_records = termination_records;
    uint32_t old_task_pages = task_table_pages;
    uint32_t old_term_pages = termination_table_pages;

    tasks = new_tasks;
    termination_records = new_records;
    task_capacity = new_capacity;
    task_table_pages = new_task_pages;
    termination_table_pages = new_term_pages;
    if (old_capacity && current_task_idx >= 0 && (uint32_t)current_task_idx < task_capacity) {
        current_task = &tasks[current_task_idx];
    }

    if (old_tasks) {
        pmm_free_contiguous_pages(old_tasks, old_task_pages);
    }
    if (old_records) {
        pmm_free_contiguous_pages(old_records, old_term_pages);
    }

    LOG_INFO_HEX("SCHED: Task table capacity: ", task_capacity);
    return 0;
}

static int find_free_task_slot(uint32_t first_slot) {
    while (1) {
        for (uint32_t i = first_slot; i < task_capacity; i++) {
            if (tasks[i].state == TASK_STATE_FREE) {
                return (int)i;
            }
        }

        uint32_t old_capacity = task_capacity;
        if (grow_task_tables(task_capacity + 1) < 0) {
            return -1;
        }
        first_slot = old_capacity;
    }
}

static uint64_t make_ttbr0(uint64_t *pgd, uint16_t asid) {
    return ((uint64_t)asid << TTBR_ASID_SHIFT) |
           ((uint64_t)pgd & TTBR_BADDR_MASK);
}

static tcb_t* get_tcb(uint32_t tid) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i].state != TASK_STATE_FREE && tasks[i].tid == tid) {
            return &tasks[i];
        }
    }
    return NULL;
}

uint32_t sched_task_capacity(void) {
    return task_capacity;
}

tcb_t *sched_task_at(uint32_t index) {
    if (index >= task_capacity || tasks[index].state == TASK_STATE_FREE) {
        return NULL;
    }
    return &tasks[index];
}

tcb_t *sched_find_task(uint32_t tid) {
    return get_tcb(tid);
}

tcb_t *sched_current_task(void) {
    return current_task;
}

static int ensure_cap_capacity(tcb_t *task, uint32_t min_capacity) {
    if (!task) {
        return -1;
    }
    return ocap_ensure(&task->caps, min_capacity);
}

static void clear_caps(tcb_t *task) {
    if (!task) {
        return;
    }
    ocap_clear(&task->caps);
}

static void release_cap_table(tcb_t *task) {
    if (!task) {
        return;
    }
    ocap_release_table(&task->caps);
}

static int install_cap(tcb_t *task, uint32_t type, uint32_t object_id,
                       uint64_t rights, uint64_t flags) {
    if (!task) {
        return -1;
    }
    return ocap_install(&task->caps, type, object_id, rights, flags);
}

static int install_cap_at(tcb_t *task, uint32_t cap, uint32_t type,
                          uint32_t object_id, uint64_t rights, uint64_t flags) {
    if (!task) {
        return -1;
    }
    return ocap_install_at(&task->caps, cap, type, object_id, rights, flags);
}

static void install_standard_caps(tcb_t *task) {
    if (!task || task->tid == 0) {
        return;
    }

    install_cap_at(task, CAP_SELF, OCAP_ENDPOINT, task->tid,
                   OCAP_RIGHT_CALL | OCAP_RIGHT_TRANSFER, 0);
    install_cap_at(task, CAP_NS, OCAP_ENDPOINT, NAME_SERVER_TID,
                   OCAP_RIGHT_CALL, 0);
}

uint64_t *sched_task_trap_frame(tcb_t *task) {
    uint64_t kernel_stack_top = (uint64_t)task->kernel_stack_base + TASK_STACK_SIZE;
    return (uint64_t *)(kernel_stack_top - 272);
}

static uint32_t ticks_from_ms(uint64_t ms) {
    uint64_t ticks = ms / 100;
    if (ticks == 0) ticks = 1;
    if (ticks > UINT32_MAX) ticks = UINT32_MAX;
    return (uint32_t)ticks;
}

static int map_user_frame(tcb_t *task, uint64_t va, uint64_t flags) {
    if (!task || !task->pgd || (va & (PAGE_SIZE - 1)) != 0) {
        return -1;
    }

    frame_t *frame = frame_alloc(task->tid, FRAME_FLAG_USER);
    if (!frame) {
        return -1;
    }

    if (vmm_map_page_asid(task->pgd, task->asid, va, frame->paddr, flags | VMM_FLAG_OWNED) < 0) {
        frame_unref(frame->paddr);
        return -1;
    }

    return 0;
}

static int vma_allows_access(const vma_t *vma, int write) {
    if (!vma || (vma->flags & VMA_GUARD)) {
        return 0;
    }

    if (write) {
        return (vma->flags & VMA_WRITE) != 0;
    }

    return (vma->flags & VMA_READ) != 0;
}

static vma_t *find_next_vma(vm_space_t *vm, const vma_t *vma) {
    return vma_next(vm, vma);
}

static int grow_stack_from_guard(tcb_t *task, vma_t *guard) {
    if (!task || !guard || !(guard->flags & VMA_GUARD)) {
        return -1;
    }

    vma_t *stack = find_next_vma(&task->vm, guard);
    if (!stack || !(stack->flags & VMA_STACK) || !(stack->flags & VMA_GROWS_DOWN) ||
        stack->start != guard->end || stack->committed_pages >= stack->max_pages) {
        return -1;
    }

    uint64_t new_stack_page = guard->start;
    if (map_user_frame(task, new_stack_page, VMM_FLAG_USER_RW) < 0) {
        return -1;
    }

    stack->start = new_stack_page;
    stack->committed_pages++;

    if (guard->start < PAGE_SIZE) {
        guard->end = guard->start;
    } else {
        guard->end = guard->start;
        guard->start -= PAGE_SIZE;
    }

    task->vm.last_vma = -1;
    return 0;
}

static int map_lazy_anon_page(tcb_t *task, vma_t *vma, uint64_t page_va, int write) {
    if (!task || !vma || !vma_allows_access(vma, write) ||
        !(vma->flags & VMA_LAZY) || vma->backing_type != VMA_BACKING_ANON) {
        return -1;
    }

    uint64_t ignored = 0;
    uint64_t entry = 0;
    if (vmm_query_page(task->pgd, page_va, &ignored, &entry) == 0) {
        if (entry & PTE_USER) {
            return 0;
        }
    }

    if (vma->object_id != 0) {
        uint64_t object_offset = vma->file_offset + (page_va - vma->start);
        uint64_t frame_paddr = 0;
        if (vm_object_resolve_page(vma->object_id, object_offset,
                                   task->tid, &frame_paddr) < 0 ||
            vmm_map_page_asid(task->pgd, task->asid, page_va, frame_paddr,
                              VMM_FLAG_USER_RW | VMM_FLAG_OWNED) < 0) {
            if (frame_paddr) {
                frame_unref(frame_paddr);
            }
            return -1;
        }
    } else if (map_user_frame(task, page_va, VMM_FLAG_USER_RW) < 0) {
        return -1;
    }

    vma->committed_pages++;
    return 0;
}

static uint64_t pte_flags_from_vma(const vma_t *vma) {
    uint64_t flags = VMM_FLAG_USER_CODE | VMM_FLAG_OWNED;
    if (vma && (vma->flags & VMA_WRITE)) {
        flags = VMM_FLAG_USER_RW | VMM_FLAG_OWNED;
    } else {
        flags |= PTE_READONLY;
    }
    return flags;
}

static int map_lazy_file_page(tcb_t *task, vma_t *vma, uint64_t page_va) {
    if (!task || !vma || !(vma->flags & VMA_LAZY) ||
        vma->backing_type != VMA_BACKING_FILE || vma->file_cap == 0) {
        return -1;
    }

    uint64_t ignored = 0;
    uint64_t entry = 0;
    if (vmm_query_page(task->pgd, page_va, &ignored, &entry) == 0) {
        if (entry & PTE_USER) {
            return 0;
        }
    }

    uint64_t page_offset = page_va - vma->start;
    uint64_t frame_paddr = 0;
    uint64_t map_flags = pte_flags_from_vma(vma);
    frame_t *zero_frame = 0;

    if (vma->object_id != 0) {
        uint64_t object_offset = vma->file_offset + page_offset;
        if (vm_object_resolve_page(vma->object_id, object_offset,
                                   task->tid, &frame_paddr) < 0) {
            return -1;
        }
        if (vma->flags & VMA_WRITE) {
            map_flags |= PTE_READONLY | PTE_SW_COW;
        }
    } else if (page_offset < vma->file_size) {
        const uint8_t *file_data = 0;
        uint64_t file_size = 0;
        if (initrd_get_file(vma->file_cap - 1, &file_data, &file_size) < 0 ||
            !file_data) {
            return -1;
        }
        uint64_t file_off = vma->file_offset + page_offset;
        if (file_off >= file_size ||
            page_cache_get_initrd_page(vma->file_cap - 1, file_off, &frame_paddr) < 0) {
            return -1;
        }
        if (vma->flags & VMA_WRITE) {
            map_flags |= PTE_READONLY | PTE_SW_COW;
        }
    } else {
        zero_frame = frame_alloc(task->tid, FRAME_FLAG_USER);
        if (!zero_frame) {
            return -1;
        }
        uint8_t *dst = (uint8_t *)zero_frame->paddr;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) {
            dst[i] = 0;
        }
        frame_paddr = zero_frame->paddr;
    }

    if (vmm_map_page_asid(task->pgd, task->asid, page_va, frame_paddr,
                          map_flags) < 0) {
        frame_unref(frame_paddr);
        return -1;
    }

    vma->committed_pages++;
    return 0;
}

static int break_cow_page(tcb_t *task, uint64_t page_va, uint64_t pa, uint64_t entry) {
    if (!task || !(entry & PTE_SW_COW) || !(entry & PTE_USER)) {
        return -1;
    }

    uint64_t old_page = pa & ~(PAGE_SIZE - 1);
    frame_t *old_frame = frame_from_paddr(old_page);
    if (!old_frame || old_frame->refcount == 0) {
        return -1;
    }

    if (old_frame->refcount == 1) {
        return vmm_update_page_flags_asid(task->pgd, task->asid, page_va,
                                          PTE_READONLY | PTE_SW_COW, 0);
    }

    frame_t *new_frame = frame_alloc(task->tid, FRAME_FLAG_USER);
    if (!new_frame) {
        return -1;
    }

    const uint8_t *src = (const uint8_t *)old_page;
    uint8_t *dst = (uint8_t *)new_frame->paddr;
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        dst[i] = src[i];
    }

    uint64_t flags = (entry & ~PTE_ADDR_MASK) & ~(PTE_READONLY | PTE_SW_COW);
    flags |= VMM_FLAG_OWNED;
    if (vmm_map_page_asid(task->pgd, task->asid, page_va, new_frame->paddr, flags) < 0) {
        frame_unref(new_frame->paddr);
        return -1;
    }

    return 0;
}

static int vm_handle_fault(tcb_t *task, uint64_t far, uint64_t esr, int explicit_write) {
    if (!task || !task->pgd || far == 0) {
        return -1;
    }

    uint64_t page_va = far & ~(PAGE_SIZE - 1);
    vma_t *vma = vma_find(&task->vm, far);
    if (!vma) {
        return -1;
    }

    if (vma->flags & VMA_GUARD) {
        return grow_stack_from_guard(task, vma);
    }

    int write = explicit_write;
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3F);
    if (esr != 0 && ec != EC_DABORT_LOWER_EL &&
        ec != EC_IABORT_LOWER_EL && ec != 0) {
        return -1;
    }
    if (ec == EC_DABORT_LOWER_EL && (esr & ESR_DABORT_WNR)) {
        write = 1;
    }

    if (ec == 0) {
        uint64_t existing_pa = 0;
        uint64_t existing_entry = 0;
        if (vmm_query_page(task->pgd, page_va, &existing_pa, &existing_entry) == 0 &&
            (existing_entry & PTE_USER)) {
            return -1;
        }
        if ((vma->flags & VMA_LAZY) == 0) {
            return -1;
        }
    } else if (ec == EC_IABORT_LOWER_EL) {
        if ((vma->flags & VMA_EXEC) == 0) {
            return -1;
        }
    } else if (!vma_allows_access(vma, write)) {
        return -1;
    }

    uint64_t pa = 0;
    uint64_t entry = 0;
    if (write && vmm_query_page(task->pgd, page_va, &pa, &entry) == 0 &&
        (entry & PTE_SW_COW)) {
        return break_cow_page(task, page_va, pa, entry);
    }

    if ((vma->flags & VMA_LAZY) && vma->backing_type == VMA_BACKING_FILE) {
        return map_lazy_file_page(task, vma, page_va);
    }

    return map_lazy_anon_page(task, vma, page_va, write);
}

int sched_resolve_task_page(tcb_t *task, uint64_t user_va, int write) {
    if (!task || !task->pgd) {
        return -1;
    }

    uint64_t pa = 0;
    uint64_t entry = 0;
    if (vmm_query_page(task->pgd, user_va, &pa, &entry) == 0) {
        if (entry & PTE_USER) {
            vma_t *vma = vma_find(&task->vm, user_va);
            return vma_allows_access(vma, write) ? 0 : -1;
        }
    }

    return vm_handle_fault(task, user_va, 0, write);
}

static void invalidate_reply_caps_for_caller(uint32_t caller_tid) {
    if (caller_tid == 0) {
        return;
    }

    for (uint32_t i = 0; i < task_capacity; i++) {
        tcb_t *task = &tasks[i];
        if (task->state == TASK_STATE_FREE || !task->caps.entries) {
            continue;
        }

        for (uint32_t cap = 1; cap < task->caps.capacity; cap++) {
            if (task->caps.entries[cap].type == OCAP_REPLY &&
                task->caps.entries[cap].object_id == caller_tid) {
                ocap_revoke_slot(&task->caps, cap);
            }
        }
    }
}

static void wake_ipc_peers(uint32_t tid) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if ((tasks[i].state == TASK_STATE_BLOCKED_ON_IPC_CALL ||
             tasks[i].state == TASK_STATE_BLOCKED_ON_IPC_REPLY) &&
            tasks[i].ipc_target_tid == tid) {
            uint64_t *tf = sched_task_trap_frame(&tasks[i]);
            tf[0] = (uint64_t)-1;
            tasks[i].state = TASK_STATE_READY;
            tasks[i].ipc_target_tid = 0;
        } else if (tasks[i].state == TASK_STATE_BLOCKED_ON_IPC_RECV &&
                   tasks[i].ipc_target_tid == tid) {
            uint64_t *tf = sched_task_trap_frame(&tasks[i]);
            tf[0] = (uint64_t)-1;
            tasks[i].state = TASK_STATE_READY;
            tasks[i].ipc_target_tid = 0;
        }
    }
}

static void clear_termination_record(termination_record_t *record) {
    record->present = 0;
    record->parent_tid = 0;
    record->tid = 0;
    record->reason = 0;
    record->exit_code = 0;
    record->esr = 0;
    record->elr = 0;
    record->far = 0;
}

static void write_wait_result(uint64_t *regs, const termination_record_t *record) {
    regs[0] = 0;
    regs[1] = record->tid;
    regs[2] = record->reason;
    regs[3] = (uint64_t)record->exit_code;
    regs[4] = record->esr;
    regs[5] = record->elr;
    regs[6] = record->far;
}

static void write_wait_error(uint64_t *regs, int code) {
    if (!regs) return;
    regs[0] = (uint64_t)(-code);
    regs[1] = 0;
    regs[2] = 0;
    regs[3] = 0;
    regs[4] = 0;
    regs[5] = 0;
    regs[6] = 0;
}

static void write_wait_running(uint64_t *regs, uint32_t tid) {
    regs[0] = 1;
    regs[1] = tid;
    regs[2] = 0;
    regs[3] = 0;
    regs[4] = 0;
    regs[5] = 0;
    regs[6] = 0;
}

static termination_record_t *find_termination_record(uint32_t parent_tid, uint32_t tid) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (termination_records[i].present &&
            termination_records[i].parent_tid == parent_tid &&
            (tid == 0 || termination_records[i].tid == tid)) {
            return &termination_records[i];
        }
    }
    return NULL;
}

static int has_live_child(uint32_t parent_tid, uint32_t tid) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i].state != TASK_STATE_FREE &&
            tasks[i].parent_tid == parent_tid &&
            (tid == 0 || tasks[i].tid == tid)) {
            return 1;
        }
    }
    return 0;
}

static void wake_waiting_parent(termination_record_t *record) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i].state == TASK_STATE_BLOCKED_ON_WAIT &&
            tasks[i].tid == record->parent_tid &&
            (tasks[i].wait_target_tid == 0 || tasks[i].wait_target_tid == record->tid)) {
            uint64_t *tf = sched_task_trap_frame(&tasks[i]);
            write_wait_result(tf, record);
            tasks[i].state = TASK_STATE_READY;
            tasks[i].wait_target_tid = 0;
            clear_termination_record(record);
            return;
        }
    }
}

static void record_task_termination(tcb_t *task, uint32_t reason, int exit_code,
                                    uint64_t esr, uint64_t elr, uint64_t far) {
    if (!task || task->tid == 0 || task->parent_tid == 0) {
        return;
    }

    if (!get_tcb(task->parent_tid)) {
        return;
    }

    termination_record_t *record = find_termination_record(task->parent_tid, task->tid);
    if (!record) {
        for (uint32_t i = 0; i < task_capacity; i++) {
            if (!termination_records[i].present) {
                record = &termination_records[i];
                break;
            }
        }
    }

    if (!record) {
        LOG_FAIL("SCHED: Dropping child exit status; record table full.");
        return;
    }

    record->present = 1;
    record->parent_tid = task->parent_tid;
    record->tid = task->tid;
    record->reason = reason;
    record->exit_code = exit_code;
    record->esr = esr;
    record->elr = elr;
    record->far = far;

    wake_waiting_parent(record);
}

static void destroy_task(tcb_t *task) {
    if (!task || task->state == TASK_STATE_FREE || task->tid == 0) return;

    wake_ipc_peers(task->tid);
    invalidate_reply_caps_for_caller(task->tid);
    memcap_release_for_owner(task->tid);
    memcap_forget_mappings_for_target(task->tid);

    if (task->pgd) {
        vmm_destroy_address_space(task->pgd);
    }
    vma_destroy(&task->vm);
    release_user_asid(task->asid);

    task->tid = 0;
    task->state = TASK_STATE_FREE;
    task->pgd = NULL;
    task->asid = 0;
    task->reserved_asid_padding = 0;
    task->sp = 0;
    task->sp_el0 = 0;
    task->priority = 0;
    task->base_priority = 0;
    task->quantum = 0;
    task->ticks_remaining = 0;
    task->wait_time = 0;
    task->sleep_ticks_remaining = 0;
    sched_clear_ipc_state(task);
    task->awaiting_irq = 0;
    task->parent_tid = 0;
    task->wait_target_tid = 0;
    task->user_stack_base = NULL;
    task->user_heap_pointer = 0;
    task->user_shared_pointer = 0;
    clear_caps(task);
    release_cap_table(task);
}

static int select_next_ready_task(void) {
    int best_idx = -1;
    uint8_t best_prio = 255;

    for (uint32_t offset = 1; offset <= task_capacity; offset++) {
        uint32_t i = ((uint32_t)current_task_idx + offset) % task_capacity;
        if (tasks[i].state == TASK_STATE_READY && tasks[i].priority < best_prio) {
            best_prio = tasks[i].priority;
            best_idx = i;
        }
    }

    return best_idx;
}

extern void cpu_switch_to_dead(tcb_t *next);

static void switch_to_task_address_space(tcb_t *task) {
    if (task->pgd != NULL) {
        __asm__ volatile("msr sp_el0, %0" : : "r"(task->sp_el0));
        __asm__ volatile("msr ttbr0_el1, %0" : : "r"(make_ttbr0(task->pgd, task->asid)));
    } else {
        __asm__ volatile("msr ttbr0_el1, %0" : : "r"(make_ttbr0(l1_table, 0)));
    }

    __asm__ volatile("isb");
}

static void reset_to_kernel_address_space(void) {
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"(make_ttbr0(l1_table, 0)));
    __asm__ volatile("isb");
}

static __attribute__((noreturn)) void switch_to_next_after_current_destroyed(void) {
    int best_idx = select_next_ready_task();
    if (best_idx == -1) {
        best_idx = 0;
    }

    current_task = &tasks[best_idx];
    current_task_idx = best_idx;
    current_task->state = TASK_STATE_RUNNING;
    current_task->ticks_remaining = current_task->quantum;
    current_task->priority = current_task->base_priority;
    current_task->wait_time = 0;

    switch_to_task_address_space(current_task);
    cpu_switch_to_dead(current_task);

    while (1) {
        __asm__ volatile("wfi");
    }
}

static __attribute__((noreturn)) void terminate_current_task(uint32_t reason, int exit_code,
                                                             uint64_t esr, uint64_t elr,
                                                             uint64_t far) {
    if (!current_task || current_task->tid == 0) {
        while (1) {
            __asm__ volatile("wfi");
        }
    }

    reset_to_kernel_address_space();
    record_task_termination(current_task, reason, exit_code, esr, elr, far);
    destroy_task(current_task);
    switch_to_next_after_current_destroyed();
}

void sched_init(void) {
    LOG_INFO("SCHED: Initializing Round-Robin Task Scheduler...");

    for (uint32_t i = 0; i <= MAX_USER_ASIDS; i++) {
        asid_in_use[i] = 0;
    }
    next_user_asid = 1;

    if (grow_task_tables(TASK_INITIAL_CAPACITY) < 0) {
        LOG_FAIL("SCHED: Failed to allocate task tables.");
        while (1) {
            __asm__ volatile("wfi");
        }
    }

    memcap_init();
    tasks[0].tid = 0;
    tasks[0].state = TASK_STATE_RUNNING;
    tasks[0].priority = 255;     
    tasks[0].base_priority = 255;
    tasks[0].quantum = 1;
    tasks[0].ticks_remaining = 1;
    tasks[0].wait_time = 0;
    tasks[0].wait_time = 0;
    tasks[0].ipc_target_tid = 0;
    sched_clear_ipc_state(&tasks[0]);
    tasks[0].awaiting_irq = 0;
    tasks[0].parent_tid = 0;
    tasks[0].wait_target_tid = 0;
    tasks[0].pgd = NULL;
    tasks[0].asid = 0;
    tasks[0].reserved_asid_padding = 0;
    tasks[0].sp_el0 = 0;
    tasks[0].user_heap_pointer = 0;
    tasks[0].user_shared_pointer = 0;
    clear_caps(&tasks[0]);

    current_task = &tasks[0];
    current_task_idx = 0;

    LOG_OK("SCHED: Scheduler Initialized. Kernel Boot Thread is Task 0.");
}

int sched_create_task(void (*entry_point)(void), uint8_t priority) {
    int idx = find_free_task_slot(0);
    if (idx == -1) {
        LOG_FAIL("SCHED: Cannot create task, task table growth failed.");
        return -1;
    }

    tcb_t *tcb = &tasks[idx];
    tcb->kernel_stack_base = kmalloc(TASK_STACK_SIZE);
    if (!tcb->kernel_stack_base) {
        LOG_FAIL("SCHED: Failed to allocate stack for new task.");
        return -1;
    }
    for (int i = 0; i < 31; i++) {
        tcb->registers[i] = 0;
    }
    uint64_t stack_top = (uint64_t)tcb->kernel_stack_base + TASK_STACK_SIZE;

    // The restore_all macro in vectors.S expects a 272-byte frame containing
    // all 31 general purpose registers, the Link Register (x30), and ELR/SPSR.
    tcb->sp = stack_top - 272;

    uint64_t *trap_frame = (uint64_t *)tcb->sp;
    for (int i = 0; i < 34; i++) {
        trap_frame[i] = 0;
    }

    trap_frame[32] = (uint64_t)entry_point;
    trap_frame[33] = 0x345;                
    
    extern void ret_from_fork(void);
    tcb->registers[30] = (uint64_t)ret_from_fork;

    tcb->tid = next_tid++;
    tcb->state = TASK_STATE_READY;

    tcb->priority = priority;
    tcb->base_priority = priority;
    if (priority <= 10) tcb->quantum = 10;
    else if (priority <= 50) tcb->quantum = 5;
    else tcb->quantum = 2;

    tcb->ticks_remaining = tcb->quantum;
    sched_clear_ipc_state(tcb);
    tcb->awaiting_irq = 0;
    tcb->parent_tid = 0;
    tcb->wait_target_tid = 0;
    tcb->pgd = NULL;
    tcb->asid = 0;
    tcb->reserved_asid_padding = 0;
    tcb->sp_el0 = 0;
    tcb->vm.vmas = NULL;
    tcb->vm.count = 0;
    tcb->vm.capacity = 0;
    tcb->vm.table_pages = 0;
    tcb->vm.last_vma = -1;
    tcb->user_heap_pointer = 0;
    tcb->user_shared_pointer = 0;
    clear_caps(tcb);
    install_standard_caps(tcb);

    LOG_INFO_HEX("SCHED: Created new kernel task. TID = ", tcb->tid);
    
    return tcb->tid;
}

static int create_user_task_with_file_cap(const uint8_t *elf_data, uint64_t elf_size,
                                          uint8_t priority, uint32_t file_cap) {
    int idx = find_free_task_slot(1);
    if (idx == -1) {
        LOG_FAIL("SCHED: Cannot create user task, task table growth failed.");
        return -1;
    }

    tcb_t *tcb = &tasks[idx];
    tcb->tid = 0;
    tcb->state = TASK_STATE_FREE;
    tcb->pgd = NULL;
    if (allocate_user_asid(&tcb->asid) < 0) {
        LOG_FAIL("SCHED: Failed to allocate ASID for user task.");
        return -1;
    }
    tcb->reserved_asid_padding = 0;
    tcb->user_stack_base = NULL;
    tcb->user_heap_pointer = 0;
    tcb->user_shared_pointer = 0;
    tcb->vm.vmas = NULL;
    tcb->vm.count = 0;
    tcb->vm.capacity = 0;
    tcb->vm.table_pages = 0;
    tcb->vm.last_vma = -1;
    if (!tcb->kernel_stack_base) {
        tcb->kernel_stack_base = kmalloc(TASK_STACK_SIZE);
        if (!tcb->kernel_stack_base) {
            release_user_asid(tcb->asid);
            tcb->asid = 0;
            return -1;
        }
    }
    
    uint64_t kernel_stack_top = (uint64_t)tcb->kernel_stack_base + TASK_STACK_SIZE;
    
    // Set up the exact 272-byte Trap Frame expected by vectors.S restore_all!
    tcb->sp = kernel_stack_top - 272; 
    uint64_t *trap_frame = (uint64_t *)tcb->sp;
    for (int i = 0; i < 34; i++) trap_frame[i] = 0;
    for (int i = 0; i < 31; i++) tcb->registers[i] = 0;

    extern void ret_from_fork(void);
    tcb->registers[30] = (uint64_t)ret_from_fork;

    if (vma_init(&tcb->vm) < 0) {
        LOG_FAIL("SCHED: Failed to allocate VMA table.");
        release_user_asid(tcb->asid);
        tcb->asid = 0;
        return -1;
    }
    tcb->pgd = vmm_create_address_space();
    if (!tcb->pgd) goto fail;

    tcb->tid = next_tid++;
    tcb->priority = priority;
    tcb->base_priority = priority;
    if (priority <= 10) tcb->quantum = 10;
    else if (priority <= 50) tcb->quantum = 5;
    else tcb->quantum = 2;
    tcb->ticks_remaining = tcb->quantum;
    tcb->wait_time = 0;
    
    sched_clear_ipc_state(tcb);
    tcb->awaiting_irq = 0;
    tcb->parent_tid = 0;
    tcb->wait_target_tid = 0;
    clear_caps(tcb);
    install_standard_caps(tcb);
    tcb->user_heap_pointer = 0xA0000000;
    tcb->user_shared_pointer = USER_SHARED_BASE;
    uint64_t entry_point = elf_load(elf_data, elf_size, tcb->pgd, tcb->asid, &tcb->vm, file_cap);
    if (entry_point == 0) {
        LOG_FAIL("SCHED: ELF loading failed!");
        goto fail;
    }
    uint64_t *tf = (uint64_t *)tcb->sp;
    tf[32] = entry_point;
    tf[33] = 0x0;

    if (vma_add_ex(&tcb->vm, USER_STACK_GUARD_BASE, USER_STACK_BASE,
                   VMA_USER | VMA_GUARD, 0, 0, 0,
                   VMA_BACKING_NONE, 0, 0, 1) < 0 ||
        vma_add_ex(&tcb->vm, USER_STACK_BASE, USER_STACK_TOP,
                   VMA_USER | VMA_READ | VMA_WRITE | VMA_STACK | VMA_GROWS_DOWN,
                   0, 0, 0, VMA_BACKING_ANON, 0, 1,
                   USER_STACK_MAX_SIZE / PAGE_SIZE) < 0) {
        LOG_FAIL("SCHED: Failed to register user stack VMA.");
        goto fail;
    }
    frame_t *stack_frame = frame_alloc(tcb->tid, FRAME_FLAG_USER);
    if (!stack_frame) {
        LOG_FAIL("SCHED: Failed to allocate user stack.");
        goto fail;
    }
    // USER_STACK_GUARD_BASE is intentionally left unmapped.
    if (vmm_map_page_asid(tcb->pgd, tcb->asid, USER_STACK_BASE, stack_frame->paddr, VMM_FLAG_USER_RW | VMM_FLAG_OWNED) < 0) {
        LOG_FAIL("SCHED: Failed to map user stack.");
        frame_unref(stack_frame->paddr);
        goto fail;
    }
    tcb->user_stack_base = (void *)stack_frame->paddr;
    tcb->sp_el0 = USER_STACK_TOP;
    tcb->state = TASK_STATE_READY;
    
    LOG_INFO_HEX("SCHED: Created new USER task (Ring 3). TID = ", tcb->tid);
    LOG_INFO_HEX("SCHED:   ELF Entry Point: ", entry_point);
    
    return tcb->tid;

fail:
    if (tcb->pgd) {
        vmm_destroy_address_space(tcb->pgd);
    }
    vma_destroy(&tcb->vm);
    release_user_asid(tcb->asid);
    tcb->tid = 0;
    tcb->state = TASK_STATE_FREE;
    tcb->pgd = NULL;
    tcb->asid = 0;
    tcb->reserved_asid_padding = 0;
    tcb->sp = 0;
    tcb->sp_el0 = 0;
    tcb->parent_tid = 0;
    tcb->wait_target_tid = 0;
    tcb->user_stack_base = NULL;
    tcb->user_heap_pointer = 0;
    tcb->user_shared_pointer = 0;
    clear_caps(tcb);
    release_cap_table(tcb);
    return -1;
}

int sched_create_user_task(const uint8_t *elf_data, uint64_t elf_size, uint8_t priority) {
    return create_user_task_with_file_cap(elf_data, elf_size, priority, 0);
}

int sched_create_user_task_from_file(const uint8_t *elf_data, uint64_t elf_size,
                                     uint8_t priority, uint32_t initrd_index) {
    return create_user_task_with_file_cap(elf_data, elf_size, priority, initrd_index + 1);
}

static int copy_vmas(vm_space_t *dst, const vm_space_t *src) {
    if (!dst || !src) {
        return -1;
    }

    for (uint32_t i = 0; i < src->count; i++) {
        const vma_t *vma = vma_get(src, i);
        if (vma && vma->object_id != 0 && vm_object_ref(vma->object_id) < 0) {
            return -1;
        }
        if (!vma || vma_add_ex(dst, vma->start, vma->end, vma->flags,
                               vma->file_offset, vma->file_size, vma->file_cap,
                               vma->backing_type, vma->object_id,
                               vma->committed_pages, vma->max_pages) < 0) {
            if (vma && vma->object_id != 0) {
                vm_object_unref(vma->object_id);
            }
            return -1;
        }
    }

    return 0;
}

static int clone_mapped_pages_cow(tcb_t *parent, tcb_t *child) {
    for (uint32_t i = 0; i < parent->vm.count; i++) {
        const vma_t *vma = vma_get(&parent->vm, i);
        if (!vma || !(vma->flags & VMA_USER) || (vma->flags & VMA_GUARD)) {
            continue;
        }

        for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
            uint64_t pa = 0;
            uint64_t entry = 0;
            if (vmm_query_page(parent->pgd, va, &pa, &entry) < 0 ||
                (entry & PTE_USER) == 0) {
                continue;
            }

            uint64_t frame_pa = pa & ~(PAGE_SIZE - 1);
            frame_t *frame = frame_from_paddr(frame_pa);
            uint64_t child_flags = entry & ~PTE_ADDR_MASK;
            int frame_backed = frame && frame->refcount > 0;

            if (frame_backed && frame_ref(frame_pa) < 0) {
                return -1;
            }

            if (frame_backed && (vma->flags & VMA_WRITE)) {
                child_flags |= PTE_READONLY | PTE_SW_COW | VMM_FLAG_OWNED;
                if (vmm_update_page_flags_asid(parent->pgd, parent->asid, va,
                                               0, PTE_READONLY | PTE_SW_COW) < 0) {
                    frame_unref(frame_pa);
                    return -1;
                }
            }

            if (!frame_backed) {
                child_flags &= ~VMM_FLAG_OWNED;
            }

            if (vmm_map_page_asid(child->pgd, child->asid, va, frame_pa, child_flags) < 0) {
                if (frame_backed) {
                    frame_unref(frame_pa);
                }
                return -1;
            }
        }
    }

    return 0;
}

int sched_fork_syscall(uint64_t *regs) {
    if (!regs || !current_task || !current_task->pgd) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int idx = find_free_task_slot(1);
    if (idx < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    tcb_t *child = &tasks[idx];
    child->tid = 0;
    child->state = TASK_STATE_FREE;
    child->pgd = NULL;
    if (allocate_user_asid(&child->asid) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }
    child->reserved_asid_padding = 0;
    child->vm.vmas = NULL;
    child->vm.count = 0;
    child->vm.capacity = 0;
    child->vm.table_pages = 0;
    child->vm.last_vma = -1;

    if (!child->kernel_stack_base) {
        child->kernel_stack_base = kmalloc(TASK_STACK_SIZE);
        if (!child->kernel_stack_base) {
            release_user_asid(child->asid);
            child->asid = 0;
            regs[0] = (uint64_t)-1;
            return -1;
        }
    }

    if (vma_init(&child->vm) < 0) {
        release_user_asid(child->asid);
        child->asid = 0;
        regs[0] = (uint64_t)-1;
        return -1;
    }

    child->pgd = vmm_create_address_space();
    if (!child->pgd) {
        vma_destroy(&child->vm);
        release_user_asid(child->asid);
        child->asid = 0;
        regs[0] = (uint64_t)-1;
        return -1;
    }

    if (copy_vmas(&child->vm, &current_task->vm) < 0 ||
        clone_mapped_pages_cow(current_task, child) < 0) {
        vmm_destroy_address_space(child->pgd);
        vma_destroy(&child->vm);
        child->pgd = NULL;
        release_user_asid(child->asid);
        child->asid = 0;
        regs[0] = (uint64_t)-1;
        return -1;
    }

    uint64_t kernel_stack_top = (uint64_t)child->kernel_stack_base + TASK_STACK_SIZE;
    child->sp = kernel_stack_top - 272;
    uint64_t *child_tf = (uint64_t *)child->sp;
    for (int i = 0; i < 34; i++) {
        child_tf[i] = regs[i];
    }
    child_tf[0] = 0;

    for (int i = 0; i < 31; i++) {
        child->registers[i] = 0;
    }
    extern void ret_from_fork(void);
    child->registers[30] = (uint64_t)ret_from_fork;

    uint64_t sp_el0 = 0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp_el0));
    current_task->sp_el0 = sp_el0;
    child->sp_el0 = sp_el0;
    child->user_stack_base = NULL;
    child->user_heap_pointer = current_task->user_heap_pointer;
    child->user_shared_pointer = current_task->user_shared_pointer;
    child->tid = next_tid++;
    child->priority = current_task->base_priority;
    child->base_priority = current_task->base_priority;
    child->quantum = current_task->quantum;
    child->ticks_remaining = child->quantum;
    child->wait_time = 0;
    child->sleep_ticks_remaining = 0;
    sched_clear_ipc_state(child);
    child->awaiting_irq = 0;
    child->parent_tid = current_task->tid;
    child->wait_target_tid = 0;
    clear_caps(child);
    install_standard_caps(child);
    if (ensure_cap_capacity(child, current_task->caps.capacity) < 0) {
        vmm_destroy_address_space(child->pgd);
        vma_destroy(&child->vm);
        release_cap_table(child);
        child->pgd = NULL;
        release_user_asid(child->asid);
        child->asid = 0;
        regs[0] = (uint64_t)-1;
        return -1;
    }
    for (uint32_t i = 3; i < current_task->caps.capacity; i++) {
        ocap_t cap = current_task->caps.entries[i];
        if (cap.type != OCAP_NONE && cap.type != OCAP_VMA && cap.type != OCAP_REPLY) {
            child->caps.entries[i] = cap;
        }
    }

    child->state = TASK_STATE_READY;
    regs[0] = child->tid;
    return (int)child->tid;
}

extern void cpu_switch_to(tcb_t *prev, tcb_t *next);

void sched_tick(void) {
    if (current_task == NULL) return;
    if (current_task->ticks_remaining > 0) {
        current_task->ticks_remaining--;
    }
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i].state == TASK_STATE_READY) {
            tasks[i].wait_time++;
            // Boost starved fucking tasks because they've been waiting forever. Exception: Leave the goddamn Idle Task (Task 0) at priority 255.
            if (tasks[i].wait_time > 100 && tasks[i].priority > 0 && tasks[i].base_priority != 255) {
                tasks[i].priority--; 
                tasks[i].wait_time = 50; 
            }
        } else if (tasks[i].state == TASK_STATE_SLEEPING) {
            if (tasks[i].sleep_ticks_remaining > 0) {
                tasks[i].sleep_ticks_remaining--;
            }
            if (tasks[i].sleep_ticks_remaining == 0) {
                tasks[i].state = TASK_STATE_READY;
                tasks[i].wait_time = 0;
            }
        } else if (tasks[i].state == TASK_STATE_BLOCKED_ON_IRQ && tasks[i].sleep_ticks_remaining > 0) {
            tasks[i].sleep_ticks_remaining--;
            if (tasks[i].sleep_ticks_remaining == 0) {
                uint64_t *tf = sched_task_trap_frame(&tasks[i]);
                tf[0] = 0;
                tasks[i].awaiting_irq = 0;
                tasks[i].state = TASK_STATE_READY;
                tasks[i].wait_time = 0;
            }
        }
    }
    int best_idx = select_next_ready_task();
    bool must_preempt = false;
    if (current_task->ticks_remaining == 0 || current_task->state != TASK_STATE_RUNNING) {
        must_preempt = true;
    }
    if (best_idx != -1 && tasks[best_idx].priority < current_task->priority) {
        must_preempt = true; 
    }
    if (!must_preempt) {
        return; 
    }

    // Exception case: We must preempt (e.g., quantum fucking expired), but absolutely NOTHING else is goddamn READY.
    if (best_idx == -1) {
        if (current_task->state == TASK_STATE_RUNNING) {
            current_task->ticks_remaining = current_task->quantum;
            current_task->priority = current_task->base_priority;
            return;
        }
        best_idx = 0;
    }
    if (current_task->state == TASK_STATE_RUNNING) {
        current_task->state = TASK_STATE_READY;
        current_task->ticks_remaining = current_task->quantum;
        current_task->priority = current_task->base_priority;
        current_task->wait_time = 0;
    }
    if (current_task->pgd != NULL) {
        uint64_t outgoing_sp_el0;
        __asm__ volatile("mrs %0, sp_el0" : "=r"(outgoing_sp_el0));
        current_task->sp_el0 = outgoing_sp_el0;
    }

    tcb_t *prev = current_task;
    current_task = &tasks[best_idx];
    current_task_idx = best_idx;
    current_task->state = TASK_STATE_RUNNING;
    current_task->wait_time = 0;

    if (prev != current_task) {
        switch_to_task_address_space(current_task);
        cpu_switch_to(prev, current_task);
    }
}

void sched_yield_syscall(void) {
    if (!current_task) return;
    current_task->ticks_remaining = 0;
    sched_tick();
}

uint64_t* sched_get_task_pgd(uint32_t tid) {
    tcb_t *t = get_tcb(tid);
    if (!t) return NULL;
    return t->pgd;
}

uint16_t sched_get_task_asid(uint32_t tid) {
    tcb_t *t = get_tcb(tid);
    if (!t) return 0;
    return t->asid;
}

void sched_mmap_syscall(uint64_t *regs, uint64_t size) {
    if (!current_task || !current_task->pgd || size == 0) {
        regs[0] = 0;
        return;
    }

    if (size > UINT64_MAX - (PAGE_SIZE - 1)) {
        regs[0] = 0;
        return;
    }
    uint64_t aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t num_pages = aligned_size / PAGE_SIZE;

    uint64_t start_va = current_task->user_heap_pointer;
    if (aligned_size > UINT64_MAX - start_va || start_va + aligned_size > VMA_USER_LIMIT) {
        regs[0] = 0;
        return;
    }

    uint32_t object_id = 0;
    if (vm_object_create_anon(current_task->tid, aligned_size,
                              VMA_READ | VMA_WRITE, &object_id) < 0) {
        regs[0] = 0;
        return;
    }

    if (vma_add_ex(&current_task->vm, start_va, start_va + aligned_size,
                   VMA_USER | VMA_READ | VMA_WRITE | VMA_MMAP | VMA_LAZY,
                   0, aligned_size, 0, VMA_BACKING_ANON, object_id,
                   0, num_pages) < 0) {
        vm_object_unref(object_id);
        regs[0] = 0;
        return;
    }

    current_task->user_heap_pointer += aligned_size;

    LOG_INFO_HEX("SCHED: mmap reserved lazy user memory at VA: ", start_va);

    regs[0] = start_va;
    return;
}

static int initrd_index_from_file_cap(tcb_t *task, uint32_t file_cap,
                                      uint64_t required_rights,
                                      uint32_t *out_index) {
    ocap_t cap;
    if (!task || !out_index ||
        ocap_lookup(&task->caps, file_cap, OCAP_FILE, required_rights, &cap) < 0) {
        return -1;
    }

    uint32_t initrd_index = cap.object_id;
    if (!initrd_get_entry(initrd_index)) {
        return -1;
    }

    *out_index = initrd_index;
    return 0;
}

int sched_file_stat_syscall(uint64_t *regs, uint32_t file_cap, uint64_t name_ptr, uint64_t name_cap) {
    if (!regs || !current_task || name_ptr == 0 || name_cap == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    uint32_t initrd_index = 0;
    if (initrd_index_from_file_cap(current_task, file_cap, OCAP_RIGHT_READ,
                                   &initrd_index) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    const initrd_entry_t *entry = initrd_get_entry(initrd_index);
    if (!entry) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    char name[INITRD_NAME_LEN];
    for (uint64_t z = 0; z < sizeof(name); z++) {
        name[z] = '\0';
    }

    uint64_t copy_len = name_cap;
    if (copy_len > sizeof(name)) {
        copy_len = sizeof(name);
    }
    if (copy_len == 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    uint64_t i = 0;
    for (; i < copy_len - 1 && i < sizeof(entry->name); i++) {
        name[i] = entry->name[i];
        if (name[i] == '\0') {
            break;
        }
    }
    if (i == copy_len - 1 || i == sizeof(entry->name) || name[i] != '\0') {
        name[copy_len - 1] = '\0';
    }

    if (copy_to_user(name_ptr, name, (size_t)copy_len) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    regs[0] = entry->size;
    return 0;
}

void sched_file_mmap_syscall(uint64_t *regs, uint32_t file_cap) {
    if (!regs || !current_task || !current_task->pgd) {
        if (regs) regs[0] = 0;
        return;
    }

    uint32_t initrd_index = 0;
    if (initrd_index_from_file_cap(current_task, file_cap, OCAP_RIGHT_MAP,
                                   &initrd_index) < 0) {
        regs[0] = 0;
        return;
    }

    const uint8_t *file_data = 0;
    uint64_t file_size = 0;
    if (initrd_get_file(initrd_index, &file_data, &file_size) < 0 || file_size == 0) {
        regs[0] = 0;
        return;
    }
    (void)file_data;

    uint64_t aligned_size = (file_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t num_pages = aligned_size / PAGE_SIZE;
    uint64_t start_va = current_task->user_heap_pointer;
    if (aligned_size > UINT64_MAX - start_va || start_va + aligned_size > VMA_USER_LIMIT) {
        regs[0] = 0;
        return;
    }

    uint32_t object_id = 0;
    if (vm_object_create_initrd(current_task->tid, initrd_index, 0, file_size,
                                aligned_size,
                                VMA_READ, &object_id) < 0) {
        regs[0] = 0;
        return;
    }

    if (vma_add_ex(&current_task->vm, start_va, start_va + aligned_size,
                   VMA_USER | VMA_READ | VMA_MMAP | VMA_FILE | VMA_LAZY,
                   0, file_size, initrd_index + 1,
                   VMA_BACKING_FILE, object_id, 0, num_pages) < 0) {
        vm_object_unref(object_id);
        regs[0] = 0;
        return;
    }

    current_task->user_heap_pointer += aligned_size;
    regs[0] = start_va;
}

void sched_sleep_syscall(uint64_t *regs, uint64_t ms) {
    (void)regs;
    if (!current_task) return;

    current_task->state = TASK_STATE_SLEEPING;
    current_task->sleep_ticks_remaining = ticks_from_ms(ms);
    current_task->ticks_remaining = 0;
    
    sched_tick();
}

int sched_spawn_syscall(uint64_t *regs, uint64_t elf_data, uint64_t elf_size, uint8_t priority) {
    if (!current_task || elf_data == 0 || elf_size == 0 || elf_size > MAX_USER_SPAWN_ELF_SIZE) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    if (user_range_readable(elf_data, (size_t)elf_size) < 0) {
        LOG_WARN("SCHED: spawn rejected invalid user ELF pointer.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int tid = sched_create_user_task((const uint8_t*)elf_data, elf_size, priority);
    if (tid >= 0) {
        tcb_t *child = get_tcb((uint32_t)tid);
        if (child) {
            child->parent_tid = current_task->tid;
        }
    }
    
    if (regs) {
        regs[0] = (uint64_t)tid;
        regs[1] = tid >= 0 ? (uint64_t)install_endpoint_cap_for_tid(current_task, (uint32_t)tid) : (uint64_t)-1;
    }
    return tid;
}

int sched_spawn_file_syscall(uint64_t *regs, uint64_t name_ptr, uint8_t priority) {
    if (!current_task || name_ptr == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    char name[64];
    if (copy_string_from_user(name, sizeof(name), name_ptr) < 0) {
        LOG_WARN("SCHED: spawn_file rejected invalid user filename pointer.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    const uint8_t *elf_data = 0;
    uint64_t elf_size = 0;
    uint32_t initrd_index = 0;
    if (initrd_find_index(name, &initrd_index) < 0 ||
        initrd_get_file(initrd_index, &elf_data, &elf_size) < 0) {
        LOG_WARN("SCHED: initrd file not found.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int tid = sched_create_user_task_from_file(elf_data, elf_size, priority, initrd_index);
    if (tid >= 0) {
        tcb_t *child = get_tcb((uint32_t)tid);
        if (child) {
            child->parent_tid = current_task->tid;
        }
    }

    if (regs) {
        regs[0] = (uint64_t)tid;
        regs[1] = tid >= 0 ? (uint64_t)install_endpoint_cap_for_tid(current_task, (uint32_t)tid) : (uint64_t)-1;
    }
    return tid;
}

int sched_install_exec_cap_at(uint32_t tid, uint32_t cap, uint32_t initrd_index) {
    tcb_t *task = get_tcb(tid);
    const uint8_t *elf_data = 0;
    uint64_t elf_size = 0;
    if (!task || cap < 3 ||
        initrd_get_file(initrd_index, &elf_data, &elf_size) < 0) {
        return -1;
    }

    return install_cap_at(task, cap, OCAP_EXEC, initrd_index,
                          OCAP_RIGHT_SPAWN | OCAP_RIGHT_TRANSFER, 0);
}

int sched_install_file_cap_at(uint32_t tid, uint32_t cap, uint32_t initrd_index) {
    tcb_t *task = get_tcb(tid);
    const initrd_entry_t *entry = initrd_get_entry(initrd_index);
    if (!task || cap < 3 || !entry) {
        return -1;
    }

    return install_cap_at(task, cap, OCAP_FILE, initrd_index,
                          OCAP_RIGHT_READ | OCAP_RIGHT_MAP | OCAP_RIGHT_TRANSFER, 0);
}

int sched_spawn_exec_syscall(uint64_t *regs, uint32_t exec_cap, uint8_t priority) {
    ocap_t cap;
    if (!current_task ||
        ocap_lookup(&current_task->caps, exec_cap, OCAP_EXEC,
                    OCAP_RIGHT_SPAWN, &cap) < 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    const uint8_t *elf_data = 0;
    uint64_t elf_size = 0;
    if (initrd_get_file(cap.object_id, &elf_data, &elf_size) < 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int tid = sched_create_user_task_from_file(elf_data, elf_size, priority,
                                               cap.object_id);
    if (tid >= 0) {
        tcb_t *child = get_tcb((uint32_t)tid);
        if (child) {
            child->parent_tid = current_task->tid;
        }
    }

    if (regs) {
        regs[0] = (uint64_t)tid;
        regs[1] = tid >= 0 ? (uint64_t)install_endpoint_cap_for_tid(current_task, (uint32_t)tid) : (uint64_t)-1;
    }
    return tid;
}

void sched_await_irq_syscall(uint64_t *regs, uint32_t irq_num) {
    (void)regs;
    if (!current_task) return;
    
    current_task->state = TASK_STATE_BLOCKED_ON_IRQ;
    current_task->awaiting_irq = irq_num;
    current_task->sleep_ticks_remaining = 0;
    current_task->ticks_remaining = 0;
    extern void gic_enable_interrupt(uint32_t int_id);
    gic_enable_interrupt(irq_num);
    
    sched_tick();
}

void sched_await_irq_timeout_syscall(uint64_t *regs, uint32_t irq_num, uint64_t timeout_ms) {
    if (!current_task) return;

    if (regs) regs[0] = 0;
    current_task->state = TASK_STATE_BLOCKED_ON_IRQ;
    current_task->awaiting_irq = irq_num;
    current_task->sleep_ticks_remaining = ticks_from_ms(timeout_ms);
    current_task->ticks_remaining = 0;

    extern void gic_enable_interrupt(uint32_t int_id);
    gic_enable_interrupt(irq_num);

    sched_tick();
}

void sched_wake_irq(uint32_t irq_num) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i].state == TASK_STATE_BLOCKED_ON_IRQ && tasks[i].awaiting_irq == irq_num) {
            uint64_t *tf = sched_task_trap_frame(&tasks[i]);
            tf[0] = 1;
            tasks[i].state = TASK_STATE_READY;
            tasks[i].awaiting_irq = 0;
            tasks[i].sleep_ticks_remaining = 0;
            if (tasks[i].priority > 0) tasks[i].priority--;
        }
    }
}

void sched_ps_syscall(uint64_t *regs, uint32_t index) {
    if (!regs) return;
    if (index >= task_capacity || tasks[index].state == TASK_STATE_FREE) {
        regs[0] = 0;
        return;
    }

    tcb_t *task = &tasks[index];
    regs[0] = 1;
    regs[1] = task->tid;
    regs[2] = task->state;
    regs[3] = ((uint64_t)task->priority << 8) | task->base_priority;
    regs[4] = ((uint64_t)task->ticks_remaining << 32) | task->wait_time;
    regs[5] = (task->state == TASK_STATE_BLOCKED_ON_WAIT) ?
              task->wait_target_tid : task->ipc_target_tid;
    regs[6] = ((uint64_t)task->parent_tid << 32) | task->awaiting_irq;
}

void sched_task_capacity_syscall(uint64_t *regs) {
    if (!regs) {
        return;
    }

    regs[0] = task_capacity;
}

void sched_capstat_syscall(uint64_t *regs, uint32_t slot) {
    if (!regs || !current_task) {
        if (regs) regs[0] = 0;
        return;
    }

    regs[0] = 0;
    regs[1] = slot;
    regs[2] = 0;
    regs[3] = 0;
    regs[4] = 0;
    regs[5] = 0;
    regs[6] = current_task->caps.capacity;

    ocap_t cap;
    if (ocap_read_slot(&current_task->caps, slot, &cap) < 0 ||
        cap.type == OCAP_NONE) {
        return;
    }

    regs[0] = 1;
    regs[2] = cap.type;
    regs[3] = cap.object_id;
    regs[4] = cap.rights;
    regs[5] = cap.flags;
}

void sched_vmmap_syscall(uint64_t *regs, uint32_t index) {
    if (!regs || !current_task || !current_task->pgd) {
        if (regs) regs[0] = 0;
        return;
    }

    const vma_t *vma = vma_get(&current_task->vm, index);
    if (!vma) {
        regs[0] = 0;
        regs[1] = 0;
        regs[2] = 0;
        regs[3] = 0;
        regs[4] = current_task->vm.count;
        regs[5] = 0;
        regs[6] = 0;
        regs[7] = 0;
        return;
    }

    regs[0] = 1;
    regs[1] = vma->start;
    regs[2] = vma->end;
    regs[3] = vma->flags;
    regs[4] = current_task->vm.count;
    regs[5] = vma->committed_pages;
    regs[6] = vma->max_pages;
    regs[7] = vma->backing_type;
}

static int task_index_from_ptr(tcb_t *task) {
    if (!task || !tasks) {
        return -1;
    }

    for (uint32_t i = 0; i < task_capacity; i++) {
        if (&tasks[i] == task) {
            return (int)i;
        }
    }
    return -1;
}

void sched_handoff_to_task(tcb_t *target) {
    if (!current_task || !target || target == current_task) {
        return;
    }

    int target_idx = task_index_from_ptr(target);
    if (target_idx < 0) {
        return;
    }

    if (current_task->pgd != NULL) {
        uint64_t outgoing_sp_el0;
        __asm__ volatile("mrs %0, sp_el0" : "=r"(outgoing_sp_el0));
        current_task->sp_el0 = outgoing_sp_el0;
    }

    tcb_t *prev = current_task;
    if (prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
        prev->ticks_remaining = prev->quantum;
        prev->priority = prev->base_priority;
    }
    current_task = target;
    current_task_idx = target_idx;
    target->state = TASK_STATE_RUNNING;
    target->wait_time = 0;
    target->ticks_remaining = target->quantum;
    switch_to_task_address_space(target);
    cpu_switch_to(prev, target);
}

static int install_endpoint_cap_for_tid(tcb_t *task, uint32_t tid) {
    if (!task || !get_tcb(tid)) {
        return -1;
    }
    return install_cap(task, OCAP_ENDPOINT, tid,
                       OCAP_RIGHT_CALL | OCAP_RIGHT_TRANSFER, 0);
}

int vm_unmap_range(tcb_t *task, uint64_t start, uint64_t size) {
    if (!task || !task->pgd || size == 0 ||
        (start & (PAGE_SIZE - 1)) != 0 ||
        (size & (PAGE_SIZE - 1)) != 0 ||
        size > UINT64_MAX - start) {
        return -1;
    }

    uint64_t end = start + size;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vmm_unmap_page_asid(task->pgd, task->asid, va);
    }

    vma_remove(&task->vm, start, end);
    return 0;
}

void sched_wait_syscall(uint64_t *regs, uint32_t tid) {
    if (!current_task || !regs) {
        write_wait_error(regs, 3);
        return;
    }

    if (tid == current_task->tid) {
        write_wait_error(regs, 2);
        return;
    }

    termination_record_t *record = find_termination_record(current_task->tid, tid);
    if (record) {
        write_wait_result(regs, record);
        clear_termination_record(record);
        return;
    }

    if (!has_live_child(current_task->tid, tid)) {
        write_wait_error(regs, 1);
        return;
    }

    current_task->state = TASK_STATE_BLOCKED_ON_WAIT;
    current_task->wait_target_tid = tid;
    current_task->ticks_remaining = 0;

    sched_tick();
}

void sched_poll_syscall(uint64_t *regs, uint32_t tid) {
    if (!current_task || !regs) {
        write_wait_error(regs, 3);
        return;
    }

    if (tid == current_task->tid) {
        write_wait_error(regs, 2);
        return;
    }

    termination_record_t *record = find_termination_record(current_task->tid, tid);
    if (record) {
        write_wait_result(regs, record);
        return;
    }

    if (has_live_child(current_task->tid, tid)) {
        write_wait_running(regs, tid);
        return;
    }

    write_wait_error(regs, 1);
}

int sched_kill_syscall(uint64_t *regs, uint32_t tid) {
    if (tid == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    tcb_t *target = get_tcb(tid);
    if (!target) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    LOG_INFO_HEX("SCHED: Killing task. TID = ", tid);
    if (regs) regs[0] = 0;

    if (target == current_task) {
        terminate_current_task(TASK_TERM_KILLED, -1, 0, 0, 0);
    }

    record_task_termination(target, TASK_TERM_KILLED, -1, 0, 0, 0);
    destroy_task(target);
    return 0;
}

int sched_current_is_user(void) {
    return current_task && current_task->state != TASK_STATE_FREE && current_task->pgd != NULL;
}

void sched_fault_current_task(uint64_t esr, uint64_t elr, uint64_t far) {
    if (!sched_current_is_user()) {
        return;
    }

    uint32_t ec = (uint32_t)((esr >> 26) & 0x3F);
    uint64_t fault_va = far;
    if (ec == EC_IABORT_LOWER_EL && far == 0) {
        fault_va = elr;
    } else if (ec == 0) {
        uint64_t elr_page = elr & ~(PAGE_SIZE - 1);
        uint64_t existing_pa = 0;
        uint64_t existing_entry = 0;
        vma_t *elr_vma = vma_find(&current_task->vm, elr);
        if (elr_vma && (elr_vma->flags & (VMA_EXEC | VMA_LAZY)) == (VMA_EXEC | VMA_LAZY) &&
            (vmm_query_page(current_task->pgd, elr_page, &existing_pa, &existing_entry) < 0 ||
             (existing_entry & PTE_USER) == 0)) {
            fault_va = elr;
        } else if (fault_va == 0) {
            fault_va = elr;
        }
    }

    if (vm_handle_fault(current_task, fault_va, esr, 0) == 0) {
        return;
    }

    uint32_t tid = current_task->tid;
    LOG_FAIL("SCHED: User task faulted; killing task.");
    LOG_INFO_HEX("SCHED: Fault TID: ", tid);
    LOG_INFO_HEX("SCHED: ESR_EL1: ", esr);
    LOG_INFO_HEX("SCHED: ELR_EL1: ", elr);
    LOG_INFO_HEX("SCHED: FAR_EL1: ", far);
    LOG_INFO_HEX("SCHED: Fault VA: ", fault_va);
    vma_t *vma = vma_find(&current_task->vm, fault_va);
    if (!vma) {
        LOG_WARN("SCHED: Fault address is outside all VMAs.");
    } else if (vma->flags & VMA_GUARD) {
        LOG_WARN("SCHED: Fault address hit a guard VMA.");
    } else {
        LOG_INFO_HEX("SCHED: Fault VMA start: ", vma->start);
        LOG_INFO_HEX("SCHED: Fault VMA end: ", vma->end);
        LOG_INFO_HEX("SCHED: Fault VMA flags: ", vma->flags);
    }

    terminate_current_task(TASK_TERM_FAULTED, -1, esr, elr, fault_va);
}

int sched_resolve_user_page(uint64_t user_va, int write) {
    return sched_current_is_user() ?
           sched_resolve_task_page(current_task, user_va, write) : -1;
}

void sched_exit_syscall(uint64_t *regs, int exit_code) {
    (void)regs;
    if (!current_task) return;

    LOG_INFO_HEX("SCHED: Task exited. TID = ", current_task->tid);
    LOG_INFO_HEX("SCHED: Exit code: ", exit_code);

    terminate_current_task(TASK_TERM_EXITED, exit_code, 0, 0, 0);
}

