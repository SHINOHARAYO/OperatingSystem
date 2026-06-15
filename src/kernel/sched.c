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
#include "vfs.h"
#include "acpi.h"
#include "block_boot.h"
#include "smp.h"
#include "spinlock.h"
#include <stdbool.h>

extern uint64_t l1_table[512];

#define MAX_USER_SPAWN_ELF_SIZE (1024 * 1024)
#define DEFAULT_NAME_SERVER_TID 1
#define TASK_INITIAL_CAPACITY 16
#define MAX_USER_ASIDS 65535
#define USER_STACK_MAX_SIZE (64 * 1024)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

#define EC_IABORT_LOWER_EL 0x20
#define EC_DABORT_LOWER_EL 0x24
#define ESR_DABORT_WNR     (1ULL << 6)
#define TTBR_ASID_SHIFT    48
#define TTBR_BADDR_MASK    0x0000FFFFFFFFF000ULL
#define SCHED_LEVELS 32
#define SCHED_TIMER_WHEEL_SIZE 1024
#define SCHED_BOOST_INTERVAL 1000
#define MAX_VFS_EXEC_OBJECTS 32
#define MAX_DMA_OBJECTS 64
#define DMA_OBJECT_MAX_PAGES 16
#define USER_UART_MMIO_VA 0xB0000000ULL
#define UART_MMIO_PA 0x09000000ULL
#define VIRTIO_MMIO_FALLBACK_PA 0x0A000000ULL
#define BOOT_DEVICE_UART  (1U << 0)
#define BOOT_DEVICE_BLOCK (1U << 1)

static tcb_t **tasks = NULL;
static uint32_t task_capacity = 0;
static uint32_t task_table_pages = 0;
static uint32_t task_slot_pages = 0;
static spinlock_t task_table_lock;
static spinlock_t asid_lock;

typedef struct {
    uint32_t core_id;
    uint8_t online;
    uint8_t reserved[3];
    spinlock_t lock;
    volatile uint32_t reschedule_pending;
    uint32_t ready_bitmap;
    uint32_t ready_count;
    uint64_t total_ticks;
    tcb_t *queues[SCHED_LEVELS];
    tcb_t *timer_wheel[SCHED_TIMER_WHEEL_SIZE];
    tcb_t *running_task;
    tcb_t *idle_task;
    int current_task_idx;
} sched_core_t;

typedef struct {
    uint64_t registers[31];
    uint64_t sp;
} sched_idle_context_t;

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

typedef struct {
    uint32_t present;
    uint32_t owner_tid;
    uint32_t fs_tid;
    uint32_t file_index;
    uint32_t boot_flags;
    uint64_t size;
    uint32_t pages;
    uint8_t *data;
} vfs_exec_object_t;

typedef struct {
    uint32_t present;
    uint32_t owner_tid;
    uint64_t user_va;
    uint64_t size;
    uint32_t page_count;
    uint64_t rights;
    uint64_t pages[DMA_OBJECT_MAX_PAGES];
} dma_object_t;

static termination_record_t *termination_records = NULL;
static uint32_t termination_table_pages = 0;
static vfs_exec_object_t vfs_exec_objects[MAX_VFS_EXEC_OBJECTS];
static dma_object_t dma_objects[MAX_DMA_OBJECTS];
static void clear_caps(tcb_t *task);
static void clear_termination_record(termination_record_t *record);
static int install_cap(tcb_t *task, uint32_t type, uint32_t object_id,
                       uint64_t rights, uint64_t flags);
static void install_standard_caps(tcb_t *task);
static int install_endpoint_cap_for_tid(tcb_t *task, uint32_t tid);
static int task_index_from_ptr(tcb_t *task);
static __attribute__((noreturn)) void terminate_current_task(uint32_t reason, int exit_code,
                                                             uint64_t esr, uint64_t elr,
                                                             uint64_t far);

static uint32_t next_tid = 1;
static uint32_t name_server_tid = DEFAULT_NAME_SERVER_TID;
static uint8_t asid_in_use[MAX_USER_ASIDS + 1];
static uint16_t next_user_asid = 1;
static sched_core_t sched_cores[MAX_SCHED_CORES];
static tcb_t secondary_idle_tasks[MAX_SCHED_CORES];
static uint32_t sched_online_cores = 1;
static uint32_t next_ready_core = 0;
static block_boot_info_t block_boot_info_page __attribute__((aligned(PAGE_SIZE)));

static uint32_t current_core_id(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static sched_core_t *sched_core_by_id(uint32_t core_id) {
    if (core_id >= MAX_SCHED_CORES || !sched_cores[core_id].online) {
        return &sched_cores[0];
    }
    return &sched_cores[core_id];
}

static sched_core_t *sched_current_core(void) {
    return sched_core_by_id(current_core_id());
}

static void sched_task_lock(tcb_t *task) {
    if (task) {
        spin_lock(&task->lock);
    }
}

static void sched_task_unlock(tcb_t *task) {
    if (task) {
        spin_unlock(&task->lock);
    }
}

static sched_core_t *sched_core_for_task(tcb_t *task) {
    if (!task || task->sched_core_id >= MAX_SCHED_CORES ||
        !sched_cores[task->sched_core_id].online) {
        return &sched_cores[0];
    }
    return &sched_cores[task->sched_core_id];
}

static uint32_t sched_core_load(sched_core_t *core) {
    if (!core || !core->online) {
        return UINT32_MAX;
    }
    spin_lock(&core->lock);
    uint32_t load = core->ready_count;
    spin_unlock(&core->lock);
    if (core->running_task && core->running_task->tid != 0 &&
        core->running_task->state == TASK_STATE_RUNNING) {
        load++;
    }
    return load;
}

static sched_core_t *sched_select_ready_core(void) {
    uint32_t online = sched_online_core_count();
    if (online == 0) {
        return &sched_cores[0];
    }

    sched_core_t *best = &sched_cores[0];
    uint32_t best_load = UINT32_MAX;
    uint32_t start = __atomic_fetch_add(&next_ready_core, 1, __ATOMIC_RELAXED);

    for (uint32_t step = 0; step < MAX_SCHED_CORES; step++) {
        uint32_t core_id = (start + step) % MAX_SCHED_CORES;
        sched_core_t *core = &sched_cores[core_id];
        if (!core->online) {
            continue;
        }
        uint32_t load = sched_core_load(core);
        if (load < best_load) {
            best = core;
            best_load = load;
        }
    }
    return best;
}

static int sched_task_can_balance(tcb_t *task) {
    return task && task->tid != 0 && task->pgd != NULL &&
           task->parent_tid > 1 && task->base_priority == 0 &&
           !task->kill_requested;
}

static void sched_set_current(sched_core_t *core, tcb_t *task, int task_idx) {
    if (!core) {
        core = &sched_cores[0];
    }
    core->running_task = task;
    core->current_task_idx = task_idx;
    if (task) {
        task->sched_core_id = core->core_id;
    }
}

#define current_task (sched_current_core()->running_task)

static void sched_remote_reschedule(uint32_t core_id) {
    if (core_id < MAX_SCHED_CORES && sched_cores[core_id].online) {
        sched_cores[core_id].reschedule_pending = 1;
        smp_send_reschedule(core_id);
    }
}

static uint8_t normalize_priority(uint8_t priority) {
    if (priority >= SCHED_LEVELS) {
        return (uint8_t)(((uint32_t)priority * (SCHED_LEVELS - 1)) / 255);
    }
    return priority;
}

static uint32_t quantum_for_priority(uint8_t priority) {
    if (priority <= 3) return 1;
    if (priority <= 7) return 2;
    if (priority <= 15) return 4;
    if (priority <= 23) return 8;
    return 16;
}

static uint32_t ready_bit(uint8_t priority) {
    return 1U << (31 - priority);
}

static void ready_queue_reset_task(tcb_t *task) {
    if (!task) return;
    task->sched_next = NULL;
    task->sched_prev = NULL;
    task->sched_queued = 0;
}

static void timer_queue_reset_task(tcb_t *task) {
    if (!task) return;
    task->timer_next = NULL;
    task->timer_prev = NULL;
    task->timer_queued = 0;
    task->wake_tick = 0;
}

static void ready_enqueue_tail_on_core(sched_core_t *core, tcb_t *task) {
    if (!task || task->tid == 0 || task->state == TASK_STATE_FREE ||
        task->state == TASK_STATE_DEAD || task->sched_queued) {
        return;
    }
    if (!core) {
        core = &sched_cores[0];
    }
    spin_lock(&core->lock);

    if (task->priority >= SCHED_LEVELS) {
        task->priority = SCHED_LEVELS - 1;
    }

    uint8_t priority = task->priority;
    tcb_t *head = core->queues[priority];
    if (!head) {
        core->queues[priority] = task;
        task->sched_next = task;
        task->sched_prev = task;
    } else {
        tcb_t *tail = head->sched_prev;
        tail->sched_next = task;
        task->sched_prev = tail;
        task->sched_next = head;
        head->sched_prev = task;
    }
    task->sched_queued = 1;
    task->sched_core_id = core->core_id;
    core->ready_count++;
    core->ready_bitmap |= ready_bit(priority);
    spin_unlock(&core->lock);
}

static void ready_enqueue_tail(tcb_t *task) {
    ready_enqueue_tail_on_core(sched_core_for_task(task), task);
}

static void ready_detach_locked(sched_core_t *core, tcb_t *task) {
    uint8_t priority = task->priority;
    if (priority >= SCHED_LEVELS) {
        priority = SCHED_LEVELS - 1;
    }

    if (task->sched_next == task) {
        core->queues[priority] = NULL;
        core->ready_bitmap &= ~ready_bit(priority);
    } else {
        if (core->queues[priority] == task) {
            core->queues[priority] = task->sched_next;
        }
        task->sched_prev->sched_next = task->sched_next;
        task->sched_next->sched_prev = task->sched_prev;
    }
    ready_queue_reset_task(task);
    if (core->ready_count > 0) {
        core->ready_count--;
    }
}

static void ready_attach_locked(sched_core_t *core, tcb_t *task) {
    uint8_t priority = task->priority;
    if (priority >= SCHED_LEVELS) {
        priority = SCHED_LEVELS - 1;
        task->priority = priority;
    }

    tcb_t *head = core->queues[priority];
    if (!head) {
        core->queues[priority] = task;
        task->sched_next = task;
        task->sched_prev = task;
    } else {
        tcb_t *tail = head->sched_prev;
        tail->sched_next = task;
        task->sched_prev = tail;
        task->sched_next = head;
        head->sched_prev = task;
    }
    task->sched_queued = 1;
    task->sched_core_id = core->core_id;
    core->ready_count++;
    core->ready_bitmap |= ready_bit(priority);
}

static void signal_remote_ready(tcb_t *task) {
    uint32_t current_core = current_core_id();
    if (task && task->sched_core_id != current_core &&
        task->sched_core_id < MAX_SCHED_CORES) {
        sched_remote_reschedule(task->sched_core_id);
    }
}

static void ready_remove(tcb_t *task) {
    if (!task || !task->sched_queued) {
        return;
    }
    sched_core_t *core = sched_core_for_task(task);
    spin_lock(&core->lock);
    ready_detach_locked(core, task);
    spin_unlock(&core->lock);
}

static tcb_t *ready_pop_highest(void) {
    sched_core_t *core = sched_current_core();
    spin_lock(&core->lock);
    uint32_t bitmap = core->ready_bitmap;
    if (bitmap == 0) {
        spin_unlock(&core->lock);
        return NULL;
    }

    uint32_t queue_index;
    __asm__ volatile("clz %w0, %w1" : "=r"(queue_index) : "r"(bitmap));
    tcb_t *task = core->queues[queue_index];
    if (!task) {
        core->ready_bitmap &= ~ready_bit((uint8_t)queue_index);
        spin_unlock(&core->lock);
        return NULL;
    }
    ready_detach_locked(core, task);
    spin_unlock(&core->lock);
    return task;
}

static tcb_t *ready_find_stealable_locked(sched_core_t *core) {
    for (int level = SCHED_LEVELS - 1; level >= 0; level--) {
        tcb_t *head = core->queues[level];
        if (!head) {
            continue;
        }

        tcb_t *task = head->sched_prev;
        tcb_t *tail = task;
        do {
            if (task->state == TASK_STATE_READY &&
                task->sched_queued && sched_task_can_balance(task)) {
                return task;
            }
            task = task->sched_prev;
        } while (task != tail);
    }
    return NULL;
}

static int ready_steal_from_core(sched_core_t *dst, sched_core_t *src) {
    if (!dst || !src || dst == src || !dst->online || !src->online) {
        return 0;
    }

    sched_core_t *first = dst->core_id < src->core_id ? dst : src;
    sched_core_t *second = first == dst ? src : dst;
    spin_lock(&first->lock);
    spin_lock(&second->lock);

    int stolen = 0;
    if (dst->ready_count == 0 && src->ready_count > 0) {
        tcb_t *task = ready_find_stealable_locked(src);
        if (task) {
            ready_detach_locked(src, task);
            ready_attach_locked(dst, task);
            stolen = 1;
        }
    }

    spin_unlock(&second->lock);
    spin_unlock(&first->lock);
    return stolen;
}

static int ready_steal_for_current_core(void) {
    sched_core_t *dst = sched_current_core();
    for (uint32_t offset = 1; offset < MAX_SCHED_CORES; offset++) {
        uint32_t core_id = (dst->core_id + offset) % MAX_SCHED_CORES;
        sched_core_t *core = &sched_cores[core_id];
        if (core == dst || !core->online) {
            continue;
        }
        if (ready_steal_from_core(dst, core)) {
            return 1;
        }
    }
    return 0;
}

static int highest_ready_priority(void) {
    sched_core_t *core = sched_current_core();
    spin_lock(&core->lock);
    uint32_t bitmap = core->ready_bitmap;
    spin_unlock(&core->lock);
    if (bitmap == 0) {
        return -1;
    }

    uint32_t queue_index;
    __asm__ volatile("clz %w0, %w1" : "=r"(queue_index) : "r"(bitmap));
    return (int)queue_index;
}

void sched_make_ready(tcb_t *task) {
    if (!task || task->tid == 0) {
        return;
    }

    task_state_t old_state = task->state;
    task->state = TASK_STATE_READY;
    task->ticks_remaining = quantum_for_priority(task->priority);
    task->quantum = task->ticks_remaining;
    task->wait_time = 0;
    if (!task->sched_queued && old_state == TASK_STATE_FREE &&
        sched_task_can_balance(task)) {
        sched_core_t *target_core = sched_select_ready_core();
        task->sched_core_id = target_core->core_id;
    } else if (task->sched_core_id >= MAX_SCHED_CORES ||
               !sched_cores[task->sched_core_id].online) {
        task->sched_core_id = current_core_id();
        if (task->sched_core_id >= MAX_SCHED_CORES ||
            !sched_cores[task->sched_core_id].online) {
            task->sched_core_id = 0;
        }
    }
    ready_enqueue_tail(task);
    signal_remote_ready(task);
}

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
    task->ipc_next = NULL;
}

static int allocate_user_asid(uint16_t *out_asid) {
    if (!out_asid) {
        return -1;
    }

    spin_lock(&asid_lock);
    for (uint32_t searched = 0; searched < MAX_USER_ASIDS; searched++) {
        uint16_t candidate = next_user_asid;
        next_user_asid = (candidate == MAX_USER_ASIDS) ? 1 : (uint16_t)(candidate + 1);

        if (!asid_in_use[candidate]) {
            asid_in_use[candidate] = 1;
            vmm_flush_asid(candidate);
            *out_asid = candidate;
            spin_unlock(&asid_lock);
            return 0;
        }
    }

    spin_unlock(&asid_lock);
    return -1;
}

static void release_user_asid(uint16_t asid) {
    if (asid == 0) {
        return;
    }

    spin_lock(&asid_lock);
    vmm_flush_asid(asid);
    smp_asid_forget(asid);
    asid_in_use[asid] = 0;
    spin_unlock(&asid_lock);
}

static void init_free_task_slot(tcb_t *task) {
    if (!task) {
        return;
    }

    task->lock.value = 0;
    task->state = TASK_STATE_FREE;
    task->tid = 0;
    task->refcount = 0;
    task->asid = 0;
    task->reserved_asid_padding = 0;
    task->vm.vmas = NULL;
    task->vm.count = 0;
    task->vm.capacity = 0;
    task->vm.table_pages = 0;
    task->vm.last_vma = -1;
    task->user_heap_pointer = 0;
    task->user_shared_pointer = 0;
    task->ipc_call_head = NULL;
    task->ipc_call_tail = NULL;
    task->priority = 0;
    task->base_priority = 0;
    task->quantum = 0;
    task->ticks_remaining = 0;
    task->wait_time = 0;
    task->sleep_ticks_remaining = 0;
    task->kill_requested = 0;
    task->sched_core_id = 0;
    ready_queue_reset_task(task);
    timer_queue_reset_task(task);
    sched_clear_ipc_state(task);
    vfs_clear_task_state(task);
    clear_caps(task);
}

static int allocate_task_pointer_table(void) {
    if (tasks) {
        return 0;
    }

    task_table_pages = table_pages_for_size(sizeof(tcb_t *), MAX_USER_ASIDS + 1);
    tasks = (tcb_t **)pmm_alloc_contiguous_pages(task_table_pages);
    if (!tasks) {
        task_table_pages = 0;
        return -1;
    }
    zero_bytes(tasks, (uint64_t)task_table_pages * PAGE_SIZE);
    task_slot_pages = table_pages_for_size(sizeof(tcb_t), 1);
    return 0;
}

static int allocate_termination_table(void) {
    if (termination_records) {
        return 0;
    }

    termination_table_pages =
        table_pages_for_size(sizeof(termination_record_t), MAX_USER_ASIDS + 1);
    termination_records =
        (termination_record_t *)pmm_alloc_contiguous_pages(termination_table_pages);
    if (!termination_records) {
        termination_table_pages = 0;
        return -1;
    }
    zero_bytes(termination_records, (uint64_t)termination_table_pages * PAGE_SIZE);
    return 0;
}

static int allocate_task_slot(uint32_t index) {
    if (index > MAX_USER_ASIDS) {
        return -1;
    }
    if (tasks[index]) {
        return 0;
    }

    tcb_t *slot = (tcb_t *)pmm_alloc_contiguous_pages(task_slot_pages);
    if (!slot) {
        return -1;
    }
    zero_bytes(slot, (uint64_t)task_slot_pages * PAGE_SIZE);
    init_free_task_slot(slot);
    tasks[index] = slot;
    return 0;
}

static int grow_task_tables(uint32_t min_capacity) {
    if (min_capacity > MAX_USER_ASIDS + 1) {
        return -1;
    }

    spin_lock(&task_table_lock);
    if (task_capacity >= min_capacity) {
        spin_unlock(&task_table_lock);
        return 0;
    }

    if (allocate_task_pointer_table() < 0 || allocate_termination_table() < 0) {
        spin_unlock(&task_table_lock);
        return -1;
    }

    while (task_capacity < min_capacity) {
        if (allocate_task_slot(task_capacity) < 0) {
            spin_unlock(&task_table_lock);
            return -1;
        }
        clear_termination_record(&termination_records[task_capacity]);
        task_capacity++;
    }

    LOG_DEBUG_HEX("SCHED: Task table capacity: ", task_capacity);
    spin_unlock(&task_table_lock);
    return 0;
}

static int find_free_task_slot(uint32_t first_slot) {
    while (1) {
        spin_lock(&task_table_lock);
        for (uint32_t i = first_slot; i < task_capacity; i++) {
            if (tasks[i] && tasks[i]->state == TASK_STATE_FREE &&
                tasks[i]->refcount == 0) {
                spin_unlock(&task_table_lock);
                return (int)i;
            }
        }
        spin_unlock(&task_table_lock);

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
    spin_lock(&task_table_lock);
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i] && tasks[i]->state != TASK_STATE_FREE &&
            tasks[i]->state != TASK_STATE_DEAD && tasks[i]->tid == tid) {
            spin_unlock(&task_table_lock);
            return tasks[i];
        }
    }
    spin_unlock(&task_table_lock);
    return NULL;
}

uint32_t sched_task_capacity(void) {
    uint32_t capacity;
    spin_lock(&task_table_lock);
    capacity = task_capacity;
    spin_unlock(&task_table_lock);
    return capacity;
}

tcb_t *sched_task_at(uint32_t index) {
    spin_lock(&task_table_lock);
    if (index >= task_capacity || !tasks[index] ||
        tasks[index]->state == TASK_STATE_FREE ||
        tasks[index]->state == TASK_STATE_DEAD) {
        spin_unlock(&task_table_lock);
        return NULL;
    }
    tcb_t *task = tasks[index];
    spin_lock(&task->lock);
    task->refcount++;
    spin_unlock(&task->lock);
    spin_unlock(&task_table_lock);
    return task;
}

tcb_t *sched_find_task(uint32_t tid) {
    return sched_task_get(tid);
}

static void recycle_dead_task_slot(tcb_t *task) {
    if (!task || task->state != TASK_STATE_DEAD || task->refcount != 0) {
        return;
    }

    task->tid = 0;
    task->state = TASK_STATE_FREE;
    task->asid = 0;
    task->reserved_asid_padding = 0;
    task->sp = 0;
    task->sp_el0 = 0;
    task->priority = 0;
    task->kill_requested = 0;
    task->base_priority = 0;
    task->quantum = 0;
    task->ticks_remaining = 0;
    task->wait_time = 0;
    task->sleep_ticks_remaining = 0;
    task->sched_core_id = 0;
    ready_queue_reset_task(task);
    timer_queue_reset_task(task);
    sched_clear_ipc_state(task);
    vfs_clear_task_state(task);
    task->awaiting_irq = 0;
    task->parent_tid = 0;
    task->wait_target_tid = 0;
    task->user_stack_base = NULL;
    task->user_heap_pointer = 0;
    task->user_shared_pointer = 0;
    task->lock.value = 0;
}

tcb_t *sched_task_get(uint32_t tid) {
    if (tid == 0) {
        return NULL;
    }

    spin_lock(&task_table_lock);
    for (uint32_t i = 0; i < task_capacity; i++) {
        tcb_t *task = tasks[i];
        if (!task) {
            continue;
        }

        spin_lock(&task->lock);
        if (task->state != TASK_STATE_FREE &&
            task->state != TASK_STATE_DEAD &&
            task->tid == tid) {
            task->refcount++;
            spin_unlock(&task->lock);
            spin_unlock(&task_table_lock);
            return task;
        }
        spin_unlock(&task->lock);
    }
    spin_unlock(&task_table_lock);
    return NULL;
}

void sched_task_put(tcb_t *task) {
    if (!task || task->tid == 0) {
        return;
    }

    int recycle = 0;
    spin_lock(&task->lock);
    if (task->refcount > 0) {
        task->refcount--;
    }
    if (task->state == TASK_STATE_DEAD && task->refcount == 0) {
        recycle = 1;
    }
    spin_unlock(&task->lock);

    if (recycle) {
        recycle_dead_task_slot(task);
    }
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

static void clear_vfs_exec_object(vfs_exec_object_t *object) {
    if (!object) {
        return;
    }
    if (object->data && object->pages) {
        pmm_free_contiguous_pages(object->data, object->pages);
    }
    object->present = 0;
    object->owner_tid = 0;
    object->fs_tid = 0;
    object->file_index = 0;
    object->boot_flags = 0;
    object->size = 0;
    object->pages = 0;
    object->data = NULL;
}

static void release_vfs_exec_object(uint32_t object_id) {
    if (object_id == 0 || object_id >= MAX_VFS_EXEC_OBJECTS) {
        return;
    }
    clear_vfs_exec_object(&vfs_exec_objects[object_id]);
}

static void release_vfs_exec_caps(tcb_t *task) {
    if (!task || !task->caps.entries) {
        return;
    }

    for (uint32_t i = 0; i < task->caps.capacity; i++) {
        ocap_t *cap = &task->caps.entries[i];
        if (cap->type == OCAP_EXEC && (cap->flags & OCAP_FLAG_VFS_EXEC)) {
            release_vfs_exec_object(cap->object_id);
        }
    }
}

static void clear_dma_object(dma_object_t *object) {
    if (!object) {
        return;
    }

    if (object->present) {
        for (uint32_t i = 0; i < object->page_count; i++) {
            if (object->pages[i]) {
                frame_unref(object->pages[i]);
            }
        }
    }

    object->present = 0;
    object->owner_tid = 0;
    object->user_va = 0;
    object->size = 0;
    object->page_count = 0;
    object->rights = 0;
    for (uint32_t i = 0; i < DMA_OBJECT_MAX_PAGES; i++) {
        object->pages[i] = 0;
    }
}

static void release_dma_object(uint32_t object_id) {
    if (object_id == 0 || object_id >= MAX_DMA_OBJECTS) {
        return;
    }
    clear_dma_object(&dma_objects[object_id]);
}

static void release_dma_caps(tcb_t *task) {
    if (!task || !task->caps.entries) {
        return;
    }

    for (uint32_t i = 0; i < task->caps.capacity; i++) {
        ocap_t *cap = &task->caps.entries[i];
        if (cap->type == OCAP_DMA) {
            release_dma_object(cap->object_id);
        }
    }
}

static void clear_caps(tcb_t *task) {
    if (!task) {
        return;
    }
    release_vfs_exec_caps(task);
    release_dma_caps(task);
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
    install_cap_at(task, CAP_NS, OCAP_ENDPOINT, name_server_tid,
                   OCAP_RIGHT_CALL, 0);
}

static void update_name_server_tid(uint32_t tid) {
    if (tid == 0 || !get_tcb(tid)) {
        return;
    }
    name_server_tid = tid;
    for (uint32_t i = 1; i < task_capacity; i++) {
        tcb_t *task = tasks[i];
        if (task->state == TASK_STATE_FREE || task->tid == 0) {
            continue;
        }
        install_cap_at(task, CAP_NS, OCAP_ENDPOINT, name_server_tid,
                       OCAP_RIGHT_CALL, 0);
    }
}

static int same_name(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t boot_flags_for_initrd_name(const char *name) {
    if (same_name(name, "uart.elf") || same_name(name, "keyboard.elf")) {
        return BOOT_DEVICE_UART;
    }
    if (same_name(name, "block.elf")) {
        return BOOT_DEVICE_BLOCK;
    }
    return 0;
}

static void apply_boot_device_grants(tcb_t *task, uint32_t flags) {
    if (!task || !task->pgd) {
        return;
    }
    if (flags & BOOT_DEVICE_UART) {
        if (vmm_map_page_asid(task->pgd, task->asid, USER_UART_MMIO_VA,
                              UART_MMIO_PA, VMM_FLAG_USER_DEVICE) < 0) {
            LOG_WARN("SCHED: failed to map UART MMIO for boot service.");
        }
    }
    if (flags & BOOT_DEVICE_BLOCK) {
        acpi_virtio_blk_info_t pci_info;
        uint64_t map_pa = VIRTIO_MMIO_FALLBACK_PA;
        uint64_t map_size = BLOCK_DEVICE_MMIO_SIZE;

        block_boot_info_page.magic = BLOCK_BOOT_MAGIC;
        block_boot_info_page.version = BLOCK_BOOT_VERSION;
        block_boot_info_page.transport = BLOCK_TRANSPORT_MMIO;
        block_boot_info_page.reserved = 0;
        block_boot_info_page.bar_va = BLOCK_DEVICE_MMIO_VA;
        block_boot_info_page.bar_size = BLOCK_DEVICE_MMIO_SIZE;
        block_boot_info_page.common_off = 0;
        block_boot_info_page.notify_off = 0;
        block_boot_info_page.isr_off = 0;
        block_boot_info_page.device_off = 0;
        block_boot_info_page.notify_multiplier = 0;
        block_boot_info_page.pci_segment = 0;
        block_boot_info_page.pci_bdf = 0;
        block_boot_info_page.reserved2 = 0;

        if (acpi_get_virtio_blk_info(&pci_info) == 0) {
            map_pa = pci_info.bar_pa;
            map_size = pci_info.bar_size;

            block_boot_info_page.transport = BLOCK_TRANSPORT_PCI_MODERN;
            block_boot_info_page.bar_size = map_size;
            block_boot_info_page.common_off = pci_info.common_off;
            block_boot_info_page.notify_off = pci_info.notify_off;
            block_boot_info_page.isr_off = pci_info.isr_off;
            block_boot_info_page.device_off = pci_info.device_off;
            block_boot_info_page.notify_multiplier = pci_info.notify_multiplier;
            block_boot_info_page.pci_segment = pci_info.segment;
            block_boot_info_page.pci_bdf =
                ((uint32_t)pci_info.bus << 8) |
                ((uint32_t)pci_info.device << 3) |
                pci_info.function;
        } else {
            LOG_WARN("SCHED: no PCI virtio-blk found; falling back to QEMU virtio-mmio window.");
        }

        for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
            if (vmm_map_page_asid(task->pgd, task->asid,
                                  BLOCK_DEVICE_MMIO_VA + off,
                                  map_pa + off,
                                  VMM_FLAG_USER_DEVICE) < 0) {
                LOG_WARN("SCHED: failed to map virtio-blk device MMIO for boot service.");
            }
        }
        if (sched_map_boot_data(task->tid, &block_boot_info_page, PAGE_SIZE,
                                BLOCK_BOOT_INFO_VA) < 0) {
            LOG_WARN("SCHED: failed to map block boot info for boot service.");
        }
    }
}

uint64_t *sched_task_trap_frame(tcb_t *task) {
    uint64_t kernel_stack_top = (uint64_t)task->kernel_stack_base + TASK_STACK_SIZE;
    return (uint64_t *)(kernel_stack_top - 272);
}

static uint32_t ticks_from_ms(uint64_t ms) {
    uint64_t ticks = ms;
    if (ticks == 0) ticks = 1;
    if (ticks > UINT32_MAX) ticks = UINT32_MAX;
    return (uint32_t)ticks;
}

static void timer_remove(tcb_t *task) {
    if (!task || !task->timer_queued) {
        return;
    }
    sched_core_t *core = sched_core_for_task(task);

    uint32_t bucket = (uint32_t)(task->wake_tick & (SCHED_TIMER_WHEEL_SIZE - 1));
    if (task->timer_prev) {
        task->timer_prev->timer_next = task->timer_next;
    } else {
        core->timer_wheel[bucket] = task->timer_next;
    }
    if (task->timer_next) {
        task->timer_next->timer_prev = task->timer_prev;
    }
    timer_queue_reset_task(task);
}

static void timer_insert_at(tcb_t *task, uint64_t wake_tick) {
    if (!task || task->state == TASK_STATE_FREE) {
        return;
    }

    timer_remove(task);
    sched_core_t *core = sched_core_for_task(task);
    task->wake_tick = wake_tick;
    uint32_t bucket = (uint32_t)(wake_tick & (SCHED_TIMER_WHEEL_SIZE - 1));
    task->timer_next = core->timer_wheel[bucket];
    task->timer_prev = NULL;
    if (task->timer_next) {
        task->timer_next->timer_prev = task;
    }
    core->timer_wheel[bucket] = task;
    task->timer_queued = 1;
}

static void timer_insert_after(tcb_t *task, uint64_t delta_ticks) {
    if (delta_ticks == 0) {
        delta_ticks = 1;
    }
    timer_insert_at(task, sched_core_for_task(task)->total_ticks + delta_ticks);
}

static int tick_has_expired(uint64_t now, uint64_t target) {
    return (int64_t)(now - target) >= 0;
}

static void process_timer_wheel_bucket(void) {
    sched_core_t *core = sched_current_core();
    uint32_t bucket = (uint32_t)(core->total_ticks & (SCHED_TIMER_WHEEL_SIZE - 1));
    tcb_t *task = core->timer_wheel[bucket];
    while (task) {
        tcb_t *next = task->timer_next;
        if (tick_has_expired(core->total_ticks, task->wake_tick)) {
            timer_remove(task);
            if (task->state == TASK_STATE_SLEEPING) {
                task->sleep_ticks_remaining = 0;
                sched_make_ready(task);
            } else if (task->state == TASK_STATE_BLOCKED_ON_IRQ) {
                uint64_t *tf = sched_task_trap_frame(task);
                tf[0] = 0;
                task->awaiting_irq = 0;
                task->sleep_ticks_remaining = 0;
                sched_make_ready(task);
            }
        }
        task = next;
    }
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
        tcb_t *task = tasks[i];
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

static void unlink_queued_ipc_call(tcb_t *caller) {
    if (!caller || caller->state != TASK_STATE_BLOCKED_ON_IPC_CALL ||
        caller->ipc_target_tid == 0) {
        return;
    }

    tcb_t *endpoint = get_tcb(caller->ipc_target_tid);
    if (!endpoint) {
        return;
    }

    sched_task_lock(endpoint);
    tcb_t *prev = NULL;
    tcb_t *cur = endpoint->ipc_call_head;
    while (cur) {
        if (cur == caller) {
            if (prev) {
                prev->ipc_next = cur->ipc_next;
            } else {
                endpoint->ipc_call_head = cur->ipc_next;
            }
            if (endpoint->ipc_call_tail == cur) {
                endpoint->ipc_call_tail = prev;
            }
            cur->ipc_next = NULL;
            sched_task_unlock(endpoint);
            return;
        }
        prev = cur;
        cur = cur->ipc_next;
    }
    sched_task_unlock(endpoint);
}

static void wake_ipc_peers(uint32_t tid) {
    tcb_t *endpoint = get_tcb(tid);
    while (endpoint) {
        sched_task_lock(endpoint);
        tcb_t *caller = endpoint->ipc_call_head;
        if (caller) {
            endpoint->ipc_call_head = caller->ipc_next;
            if (endpoint->ipc_call_tail == caller) {
                endpoint->ipc_call_tail = NULL;
            }
            caller->ipc_next = NULL;
        }
        sched_task_unlock(endpoint);
        if (!caller) {
            break;
        }
        if (caller->state == TASK_STATE_BLOCKED_ON_IPC_CALL) {
            uint64_t *tf = sched_task_trap_frame(caller);
            tf[0] = (uint64_t)-1;
            sched_clear_ipc_state(caller);
            sched_make_ready(caller);
        }
    }

    for (uint32_t i = 0; i < task_capacity; i++) {
        if ((tasks[i]->state == TASK_STATE_BLOCKED_ON_IPC_CALL ||
             tasks[i]->state == TASK_STATE_BLOCKED_ON_IPC_REPLY) &&
            tasks[i]->ipc_target_tid == tid) {
            uint64_t *tf = sched_task_trap_frame(tasks[i]);
            tf[0] = (uint64_t)-1;
            sched_clear_ipc_state(tasks[i]);
            sched_make_ready(tasks[i]);
        } else if (tasks[i]->state == TASK_STATE_BLOCKED_ON_IPC_RECV &&
                   tasks[i]->ipc_target_tid == tid) {
            uint64_t *tf = sched_task_trap_frame(tasks[i]);
            tf[0] = (uint64_t)-1;
            sched_clear_ipc_state(tasks[i]);
            sched_make_ready(tasks[i]);
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
        if (tasks[i]->state != TASK_STATE_FREE &&
            tasks[i]->state != TASK_STATE_DEAD &&
            tasks[i]->parent_tid == parent_tid &&
            (tid == 0 || tasks[i]->tid == tid)) {
            return 1;
        }
    }
    return 0;
}

static void wake_waiting_parent(termination_record_t *record) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i]->state == TASK_STATE_BLOCKED_ON_WAIT &&
            tasks[i]->tid == record->parent_tid &&
            (tasks[i]->wait_target_tid == 0 || tasks[i]->wait_target_tid == record->tid)) {
            uint64_t *tf = sched_task_trap_frame(tasks[i]);
            write_wait_result(tf, record);
            tasks[i]->wait_target_tid = 0;
            clear_termination_record(record);
            sched_make_ready(tasks[i]);
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
    if (!task || task->state == TASK_STATE_FREE ||
        task->state == TASK_STATE_DEAD || task->tid == 0) {
        return;
    }

    ready_remove(task);
    timer_remove(task);
    unlink_queued_ipc_call(task);
    wake_ipc_peers(task->tid);
    invalidate_reply_caps_for_caller(task->tid);
    vfs_task_died(task->tid);
    memcap_release_for_owner(task->tid);
    memcap_forget_mappings_for_target(task->tid);

    if (task->pgd) {
        vmm_destroy_address_space(task->pgd);
    }
    vma_destroy(&task->vm);
    release_user_asid(task->asid);

    task->state = TASK_STATE_DEAD;
    task->pgd = NULL;
    task->asid = 0;
    task->reserved_asid_padding = 0;
    task->sp = 0;
    task->sp_el0 = 0;
    task->priority = 0;
    task->kill_requested = 0;
    task->base_priority = 0;
    task->quantum = 0;
    task->ticks_remaining = 0;
    task->wait_time = 0;
    task->sleep_ticks_remaining = 0;
    ready_queue_reset_task(task);
    timer_queue_reset_task(task);
    task->ipc_call_head = NULL;
    task->ipc_call_tail = NULL;
    sched_clear_ipc_state(task);
    vfs_clear_task_state(task);
    task->awaiting_irq = 0;
    task->parent_tid = 0;
    task->wait_target_tid = 0;
    task->user_stack_base = NULL;
    task->user_heap_pointer = 0;
    task->user_shared_pointer = 0;
    clear_caps(task);
    release_cap_table(task);
    sched_task_put(task);
}

extern void cpu_switch_to_dead(tcb_t *next);
extern void cpu_switch_to(tcb_t *prev, tcb_t *next);

static void switch_to_task_address_space(tcb_t *task) {
    if (task->pgd != NULL) {
        __asm__ volatile("msr sp_el0, %0" : : "r"(task->sp_el0));
        smp_asid_activate(task->asid);
        __asm__ volatile("dsb ishst" : : : "memory");
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

static void priority_boost_current_core(void) {
    sched_core_t *core = sched_current_core();
    tcb_t *boosted_head = NULL;
    spin_lock(&core->lock);
    for (uint32_t level = 0; level < SCHED_LEVELS; level++) {
        tcb_t *head = core->queues[level];
        core->queues[level] = NULL;
        if (!head) {
            continue;
        }

        tcb_t *task = head;
        do {
            tcb_t *next = task->sched_next;
            task->priority = 0;
            task->quantum = quantum_for_priority(0);
            task->ticks_remaining = task->quantum;
            task->wait_time = 0;

            if (!boosted_head) {
                boosted_head = task;
                task->sched_next = task;
                task->sched_prev = task;
            } else {
                tcb_t *tail = boosted_head->sched_prev;
                tail->sched_next = task;
                task->sched_prev = tail;
                task->sched_next = boosted_head;
                boosted_head->sched_prev = task;
            }
            task = next;
        } while (task != head);
    }
    core->queues[0] = boosted_head;
    core->ready_bitmap = boosted_head ? ready_bit(0) : 0;
    tcb_t *running = core->running_task;
    if (running && running->tid != 0 && running->state == TASK_STATE_RUNNING) {
        running->priority = 0;
        running->quantum = quantum_for_priority(0);
        running->ticks_remaining = running->quantum;
        running->wait_time = 0;
    }
    spin_unlock(&core->lock);
}

static tcb_t *ready_pop_or_steal(void) {
    tcb_t *task = ready_pop_highest();
    if (!task && ready_steal_for_current_core()) {
        task = ready_pop_highest();
    }
    return task;
}

static void schedule_next(void) {
    sched_core_t *core = sched_current_core();
    tcb_t *running = core->running_task;
    current_task = running;
    if (!tasks || !running) {
        return;
    }

    tcb_t *target = ready_pop_or_steal();
    if (!target) {
        target = core->idle_task ? core->idle_task : tasks[0];
    }

    int target_idx = task_index_from_ptr(target);
    if (target_idx < 0 && target != core->idle_task) {
        return;
    }

    tcb_t *prev = running;
    if (target == prev) {
        target->state = TASK_STATE_RUNNING;
        if (target->ticks_remaining == 0) {
            target->ticks_remaining = quantum_for_priority(target->priority);
            target->quantum = target->ticks_remaining;
        }
        return;
    }

    if (prev->pgd != NULL) {
        uint64_t outgoing_sp_el0;
        __asm__ volatile("mrs %0, sp_el0" : "=r"(outgoing_sp_el0));
        prev->sp_el0 = outgoing_sp_el0;
    }
    if (prev->tid == 0 && prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }

    sched_set_current(core, target, target_idx);
    target->state = TASK_STATE_RUNNING;
    target->wait_time = 0;
    if (target->ticks_remaining == 0) {
        target->ticks_remaining = quantum_for_priority(target->priority);
        target->quantum = target->ticks_remaining;
    }

    switch_to_task_address_space(target);
    cpu_switch_to(prev, target);
}

void sched_reschedule(void) {
    sched_core_t *core = sched_current_core();
    current_task = core->running_task;
    if (!current_task) {
        return;
    }

    if (current_task->state == TASK_STATE_RUNNING) {
        int highest = highest_ready_priority();
        if (highest < 0 || highest >= current_task->priority) {
            return;
        }
        current_task->state = TASK_STATE_READY;
        current_task->ticks_remaining = quantum_for_priority(current_task->priority);
        current_task->quantum = current_task->ticks_remaining;
        ready_enqueue_tail(current_task);
    }

    schedule_next();
}

static __attribute__((noreturn)) void switch_to_next_after_current_destroyed(void) {
    sched_core_t *core = sched_current_core();
    tcb_t *target = ready_pop_or_steal();
    if (!target) {
        target = core->idle_task ? core->idle_task : tasks[0];
    }

    int target_idx = task_index_from_ptr(target);
    if (target_idx < 0 && target != core->idle_task) {
        target_idx = 0;
        target = tasks[0];
    }
    sched_set_current(core, target, target_idx);
    current_task->state = TASK_STATE_RUNNING;
    current_task->ticks_remaining = quantum_for_priority(current_task->priority);
    current_task->quantum = current_task->ticks_remaining;
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

static int sched_take_kill_request(tcb_t *task) {
    int requested = 0;
    sched_task_lock(task);
    if (task && task->kill_requested) {
        task->kill_requested = 0;
        requested = 1;
    }
    sched_task_unlock(task);
    return requested;
}

static void sched_request_remote_kill(tcb_t *target) {
    sched_task_lock(target);
    if (target) {
        target->kill_requested = 1;
    }
    sched_task_unlock(target);

    for (uint32_t core_id = 0; core_id < MAX_SCHED_CORES; core_id++) {
        sched_remote_reschedule(core_id);
    }
}

static void sched_terminate_if_kill_requested(void) {
    if (current_task && sched_take_kill_request(current_task)) {
        terminate_current_task(TASK_TERM_KILLED, -1, 0, 0, 0);
    }
}

static void sched_process_owner_kills(void) {
    sched_core_t *core = sched_current_core();
    uint32_t capacity = task_capacity;

    for (uint32_t i = 0; i < capacity; i++) {
        tcb_t *task = tasks[i];
        if (!task || task == core->running_task ||
            task->sched_core_id != core->core_id) {
            continue;
        }

        int destroy = 0;
        sched_task_lock(task);
        if (task->kill_requested &&
            task->state != TASK_STATE_FREE &&
            task->state != TASK_STATE_DEAD &&
            task->state != TASK_STATE_RUNNING) {
            task->kill_requested = 0;
            destroy = 1;
        }
        sched_task_unlock(task);

        if (destroy) {
            record_task_termination(task, TASK_TERM_KILLED, -1, 0, 0, 0);
            destroy_task(task);
        }
    }
}

static void init_idle_task(tcb_t *idle, uint32_t core_id) {
    if (!idle) {
        return;
    }
    idle->lock.value = 0;
    idle->tid = 0;
    idle->state = TASK_STATE_RUNNING;
    idle->refcount = 1;
    idle->priority = SCHED_LEVELS - 1;
    idle->base_priority = SCHED_LEVELS - 1;
    idle->quantum = quantum_for_priority(idle->priority);
    idle->ticks_remaining = idle->quantum;
    idle->sched_core_id = core_id;
    idle->pgd = NULL;
    idle->asid = 0;
    ready_queue_reset_task(idle);
    timer_queue_reset_task(idle);
}

void sched_init(void) {
    LOG_DEBUG("SCHED: Initializing O(1) MLFQ Scheduler...");

    for (uint32_t i = 0; i <= MAX_USER_ASIDS; i++) {
        asid_in_use[i] = 0;
    }
    next_user_asid = 1;
    zero_bytes(sched_cores, sizeof(sched_cores));
    sched_online_cores = 1;
    for (uint32_t i = 0; i < MAX_SCHED_CORES; i++) {
        sched_cores[i].core_id = i;
        sched_cores[i].current_task_idx = -1;
        sched_cores[i].idle_task = NULL;
    }
    sched_cores[0].online = 1;

    if (grow_task_tables(TASK_INITIAL_CAPACITY) < 0) {
        LOG_FAIL("SCHED: Failed to allocate task tables.");
        while (1) {
            __asm__ volatile("wfi");
        }
    }

    memcap_init();
    tasks[0]->lock.value = 0;
    tasks[0]->tid = 0;
    tasks[0]->state = TASK_STATE_RUNNING;
    tasks[0]->refcount = 1;
    tasks[0]->priority = SCHED_LEVELS - 1;
    tasks[0]->base_priority = SCHED_LEVELS - 1;
    tasks[0]->quantum = quantum_for_priority(tasks[0]->priority);
    tasks[0]->ticks_remaining = tasks[0]->quantum;
    tasks[0]->wait_time = 0;
    tasks[0]->sleep_ticks_remaining = 0;
    tasks[0]->sched_core_id = 0;
    ready_queue_reset_task(tasks[0]);
    timer_queue_reset_task(tasks[0]);
    tasks[0]->ipc_target_tid = 0;
    sched_clear_ipc_state(tasks[0]);
    vfs_clear_task_state(tasks[0]);
    tasks[0]->awaiting_irq = 0;
    tasks[0]->parent_tid = 0;
    tasks[0]->wait_target_tid = 0;
    tasks[0]->pgd = NULL;
    tasks[0]->asid = 0;
    tasks[0]->reserved_asid_padding = 0;
    tasks[0]->sp_el0 = 0;
    tasks[0]->user_heap_pointer = 0;
    tasks[0]->user_shared_pointer = 0;
    clear_caps(tasks[0]);

    sched_cores[0].idle_task = tasks[0];
    sched_set_current(&sched_cores[0], tasks[0], 0);

    LOG_OK_HEX("SCHED: Scheduler Initialized. Online cores: ", sched_online_cores);
}

void sched_secondary_core_online(uint32_t core_id) {
    if (core_id == 0 || core_id >= MAX_SCHED_CORES) {
        return;
    }

    sched_core_t *core = &sched_cores[core_id];
    if (core->online) {
        return;
    }
    core->core_id = core_id;
    core->online = 1;
    init_idle_task(&secondary_idle_tasks[core_id], core_id);
    core->idle_task = &secondary_idle_tasks[core_id];
    sched_set_current(core, core->idle_task, -1);
    __atomic_add_fetch(&sched_online_cores, 1, __ATOMIC_SEQ_CST);
}

uint32_t sched_online_core_count(void) {
    return __atomic_load_n(&sched_online_cores, __ATOMIC_SEQ_CST);
}

void sched_handle_reschedule_ipi(void) {
    sched_core_t *core = sched_current_core();
    core->reschedule_pending = 0;
    sched_terminate_if_kill_requested();
    sched_process_owner_kills();
    sched_reschedule();
}

int sched_create_task(void (*entry_point)(void), uint8_t priority) {
    int idx = find_free_task_slot(0);
    if (idx == -1) {
        LOG_FAIL("SCHED: Cannot create task, task table growth failed.");
        return -1;
    }

    tcb_t *tcb = tasks[idx];
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
    tcb->state = TASK_STATE_FREE;
    tcb->refcount = 1;
    tcb->priority = normalize_priority(priority);
    tcb->base_priority = tcb->priority;
    tcb->quantum = quantum_for_priority(tcb->priority);
    tcb->ticks_remaining = tcb->quantum;
    tcb->wait_time = 0;
    tcb->sleep_ticks_remaining = 0;
    tcb->kill_requested = 0;
    ready_queue_reset_task(tcb);
    timer_queue_reset_task(tcb);
    sched_clear_ipc_state(tcb);
    vfs_clear_task_state(tcb);
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
    sched_make_ready(tcb);

    LOG_DEBUG_HEX("SCHED: Created new kernel task. TID = ", tcb->tid);
    
    return tcb->tid;
}

static int create_user_task_with_file_cap(const uint8_t *elf_data, uint64_t elf_size,
                                          uint8_t priority, uint32_t file_cap,
                                          uint32_t parent_tid) {
    int idx = find_free_task_slot(1);
    if (idx == -1) {
        LOG_FAIL("SCHED: Cannot create user task, task table growth failed.");
        return -1;
    }

    tcb_t *tcb = tasks[idx];
    tcb->tid = 0;
    tcb->state = TASK_STATE_FREE;
    tcb->refcount = 1;
    tcb->pgd = NULL;
    if (allocate_user_asid(&tcb->asid) < 0) {
        LOG_FAIL("SCHED: Failed to allocate ASID for user task.");
        tcb->refcount = 0;
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
            tcb->refcount = 0;
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
    tcb->priority = normalize_priority(priority);
    tcb->base_priority = tcb->priority;
    tcb->quantum = quantum_for_priority(tcb->priority);
    tcb->ticks_remaining = tcb->quantum;
    tcb->wait_time = 0;
    tcb->sleep_ticks_remaining = 0;
    tcb->kill_requested = 0;
    ready_queue_reset_task(tcb);
    timer_queue_reset_task(tcb);
    
    sched_clear_ipc_state(tcb);
    vfs_clear_task_state(tcb);
    tcb->awaiting_irq = 0;
    tcb->parent_tid = parent_tid;
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
    sched_make_ready(tcb);
    
    LOG_DEBUG_HEX("SCHED: Created new user task. TID = ", tcb->tid);
    LOG_DEBUG_HEX("SCHED:   ELF entry point: ", entry_point);
    
    return tcb->tid;

fail:
    if (tcb->pgd) {
        vmm_destroy_address_space(tcb->pgd);
    }
    vma_destroy(&tcb->vm);
    release_user_asid(tcb->asid);
    tcb->tid = 0;
    tcb->state = TASK_STATE_FREE;
    tcb->refcount = 0;
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
    return create_user_task_with_file_cap(elf_data, elf_size, priority, 0, 0);
}

int sched_create_user_task_from_file(const uint8_t *elf_data, uint64_t elf_size,
                                     uint8_t priority, uint32_t initrd_index) {
    return create_user_task_with_file_cap(elf_data, elf_size, priority,
                                          initrd_index + 1, 0);
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

    tcb_t *child = tasks[idx];
    child->tid = 0;
    child->state = TASK_STATE_FREE;
    child->refcount = 1;
    child->pgd = NULL;
    if (allocate_user_asid(&child->asid) < 0) {
        child->refcount = 0;
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
            child->refcount = 0;
            regs[0] = (uint64_t)-1;
            return -1;
        }
    }

    if (vma_init(&child->vm) < 0) {
        release_user_asid(child->asid);
        child->asid = 0;
        child->refcount = 0;
        regs[0] = (uint64_t)-1;
        return -1;
    }

    child->pgd = vmm_create_address_space();
    if (!child->pgd) {
        vma_destroy(&child->vm);
        release_user_asid(child->asid);
        child->asid = 0;
        child->refcount = 0;
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
        child->refcount = 0;
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
    child->priority = current_task->priority;
    child->base_priority = current_task->base_priority;
    child->quantum = quantum_for_priority(child->priority);
    child->ticks_remaining = child->quantum;
    child->wait_time = 0;
    child->sleep_ticks_remaining = 0;
    child->kill_requested = 0;
    ready_queue_reset_task(child);
    timer_queue_reset_task(child);
    sched_clear_ipc_state(child);
    vfs_clear_task_state(child);
    for (uint32_t i = 0; i < MAX_VFS_ROUTES; i++) {
        child->vfs_ids[i] = current_task->vfs_ids[i];
        child->vfs_tids[i] = current_task->vfs_tids[i];
    }
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

    sched_make_ready(child);
    regs[0] = child->tid;
    return (int)child->tid;
}

void sched_tick(void) {
    sched_core_t *core = sched_current_core();
    current_task = core->running_task;
    if (current_task == NULL) return;
    sched_terminate_if_kill_requested();
    sched_process_owner_kills();
    core->total_ticks++;
    process_timer_wheel_bucket();

    if ((core->total_ticks % SCHED_BOOST_INTERVAL) == 0) {
        priority_boost_current_core();
    }

    if (current_task->state != TASK_STATE_RUNNING) {
        schedule_next();
        return;
    }

    if (current_task->tid != 0 && current_task->ticks_remaining > 0) {
        current_task->ticks_remaining--;
    }

    int highest = highest_ready_priority();
    if (current_task->tid != 0 && current_task->ticks_remaining == 0) {
        if (current_task->priority < SCHED_LEVELS - 1) {
            current_task->priority++;
        }
        current_task->quantum = quantum_for_priority(current_task->priority);
        current_task->ticks_remaining = current_task->quantum;
        current_task->state = TASK_STATE_READY;
        current_task->wait_time = 0;
        ready_enqueue_tail(current_task);
        signal_remote_ready(current_task);
        schedule_next();
    } else if (highest >= 0 &&
               (current_task->tid == 0 || highest < current_task->priority)) {
        if (current_task->tid != 0) {
            current_task->state = TASK_STATE_READY;
            ready_enqueue_tail(current_task);
            signal_remote_ready(current_task);
        }
        schedule_next();
    }
}

void sched_yield_syscall(void) {
    current_task = sched_current_core()->running_task;
    if (!current_task) return;
    sched_terminate_if_kill_requested();
    if (current_task->tid != 0 && current_task->state == TASK_STATE_RUNNING) {
        current_task->ticks_remaining = quantum_for_priority(current_task->priority);
        current_task->quantum = current_task->ticks_remaining;
        current_task->state = TASK_STATE_READY;
        ready_enqueue_tail(current_task);
        signal_remote_ready(current_task);
    }
    schedule_next();
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

    LOG_DEBUG_HEX("SCHED: mmap reserved lazy user memory at VA: ", start_va);

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
    timer_insert_after(current_task, current_task->sleep_ticks_remaining);
    sched_reschedule();
}

int sched_spawn_syscall(uint64_t *regs, uint64_t elf_data, uint64_t elf_size, uint8_t priority) {
    if (!current_task || elf_data == 0 || elf_size == 0 || elf_size > MAX_USER_SPAWN_ELF_SIZE) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    if (user_range_readable(elf_data, (size_t)elf_size) < 0) {
        LOG_DEBUG("SCHED: spawn rejected invalid user ELF pointer.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int tid = create_user_task_with_file_cap((const uint8_t*)elf_data, elf_size,
                                             priority, 0, current_task->tid);
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
        LOG_DEBUG("SCHED: spawn_file rejected invalid user filename pointer.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    const uint8_t *elf_data = 0;
    uint64_t elf_size = 0;
    uint32_t initrd_index = 0;
    if (initrd_find_index(name, &initrd_index) < 0 ||
        initrd_get_file(initrd_index, &elf_data, &elf_size) < 0) {
        LOG_DEBUG("SCHED: boot archive file not found.");
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    int tid = create_user_task_with_file_cap(elf_data, elf_size, priority,
                                             initrd_index + 1, current_task->tid);
    if (tid >= 0) {
        tcb_t *child = get_tcb((uint32_t)tid);
        if (child) {
            child->parent_tid = current_task->tid;
            apply_boot_device_grants(child, boot_flags_for_initrd_name(name));
        }
        if (same_name(name, "ns.elf")) {
            update_name_server_tid((uint32_t)tid);
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

int sched_map_boot_data(uint32_t tid, const void *data, uint64_t size, uint64_t user_va) {
    tcb_t *task = get_tcb(tid);
    if (!task || !task->pgd || !data || size == 0 ||
        ((uint64_t)data & (PAGE_SIZE - 1)) != 0 ||
        (user_va & (PAGE_SIZE - 1)) != 0 ||
        size > VMA_USER_LIMIT || user_va > VMA_USER_LIMIT ||
        size > VMA_USER_LIMIT - user_va) {
        return -1;
    }

    uint64_t aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (vma_add_ex(&task->vm, user_va, user_va + aligned_size,
                   VMA_USER | VMA_READ | VMA_FILE,
                   0, size, 0, VMA_BACKING_NONE, 0,
                   aligned_size / PAGE_SIZE, aligned_size / PAGE_SIZE) < 0) {
        return -1;
    }

    uint64_t src = (uint64_t)data;
    for (uint64_t off = 0; off < aligned_size; off += PAGE_SIZE) {
        if (vmm_map_page_asid(task->pgd, task->asid, user_va + off, src + off,
                              VMM_FLAG_USER_CODE | PTE_READONLY) < 0) {
            for (uint64_t undo = 0; undo < off; undo += PAGE_SIZE) {
                vmm_unmap_page_asid(task->pgd, task->asid, user_va + undo);
            }
            vma_remove(&task->vm, user_va, user_va + aligned_size);
            return -1;
        }
    }

    return 0;
}

static int find_free_vfs_exec_object(void) {
    for (uint32_t i = 1; i < MAX_VFS_EXEC_OBJECTS; i++) {
        if (!vfs_exec_objects[i].present) {
            return (int)i;
        }
    }
    return -1;
}

int sched_vfs_exec_create_syscall(uint64_t *regs, uint32_t client_tid,
                                  uint64_t elf_data, uint64_t elf_size,
                                  uint32_t file_index, uint32_t boot_flags) {
    tcb_t *fs = current_task;
    if (!regs || !fs || !fs->pgd || client_tid == 0 || elf_data == 0 ||
        elf_size < sizeof(Elf64_Ehdr) || elf_size > MAX_USER_SPAWN_ELF_SIZE) {
        if (regs) regs[0] = (uint64_t)-1;
        return -1;
    }

    tcb_t *client = sched_find_task(client_tid);
    if (!client || client->state != TASK_STATE_BLOCKED_ON_VFS_CALL ||
        client->vfs_reply_tid != fs->tid ||
        fs->vfs_active_client_tid != client_tid ||
        fs->vfs_active_id != VFS_ID_FS) {
        regs[0] = (uint64_t)-1;
        if (client) {
            sched_task_put(client);
        }
        return -1;
    }

    if (user_range_readable(elf_data, (size_t)elf_size) < 0) {
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    uint64_t pages64 = (elf_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages64 == 0 || pages64 > UINT32_MAX) {
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    int object_id = find_free_vfs_exec_object();
    if (object_id < 0) {
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    uint8_t *copy = (uint8_t *)pmm_alloc_contiguous_pages(pages64);
    if (!copy) {
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    if (copy_from_user(copy, elf_data, (size_t)elf_size) < 0 ||
        copy[0] != 0x7f || copy[1] != 'E' || copy[2] != 'L' || copy[3] != 'F') {
        pmm_free_contiguous_pages(copy, pages64);
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    vfs_exec_object_t *object = &vfs_exec_objects[object_id];
    object->present = 1;
    object->owner_tid = client_tid;
    object->fs_tid = fs->tid;
    object->file_index = file_index;
    object->boot_flags = boot_flags;
    object->size = elf_size;
    object->pages = (uint32_t)pages64;
    object->data = copy;

    int cap_slot = install_cap(client, OCAP_EXEC, (uint32_t)object_id,
                               OCAP_RIGHT_SPAWN, OCAP_FLAG_VFS_EXEC);
    if (cap_slot < 0) {
        clear_vfs_exec_object(object);
        regs[0] = (uint64_t)-1;
        sched_task_put(client);
        return -1;
    }

    regs[0] = (uint64_t)cap_slot;
    regs[1] = (uint64_t)object_id;
    regs[2] = elf_size;
    sched_task_put(client);
    return cap_slot;
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
    int tid = -1;
    if (cap.flags & OCAP_FLAG_VFS_EXEC) {
        if (cap.object_id == 0 || cap.object_id >= MAX_VFS_EXEC_OBJECTS ||
            !vfs_exec_objects[cap.object_id].present) {
            if (regs) regs[0] = (uint64_t)-1;
            return -1;
        }

        vfs_exec_object_t *object = &vfs_exec_objects[cap.object_id];
        elf_data = object->data;
        elf_size = object->size;
        tid = create_user_task_with_file_cap(elf_data, elf_size, priority,
                                             0, current_task->tid);
        if (tid >= 0) {
            apply_boot_device_grants(get_tcb((uint32_t)tid), object->boot_flags);
        }
    } else {
        if (initrd_get_file(cap.object_id, &elf_data, &elf_size) < 0) {
            if (regs) regs[0] = (uint64_t)-1;
            return -1;
        }
        tid = create_user_task_with_file_cap(elf_data, elf_size, priority,
                                             cap.object_id + 1, current_task->tid);
    }

    if (cap.flags & OCAP_FLAG_VFS_EXEC) {
        release_vfs_exec_object(cap.object_id);
        ocap_revoke_slot(&current_task->caps, exec_cap);
    }

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
    sched_reschedule();
}

void sched_await_irq_timeout_syscall(uint64_t *regs, uint32_t irq_num, uint64_t timeout_ms) {
    if (!current_task) return;

    if (regs) regs[0] = 0;
    current_task->state = TASK_STATE_BLOCKED_ON_IRQ;
    current_task->awaiting_irq = irq_num;
    current_task->sleep_ticks_remaining = ticks_from_ms(timeout_ms);
    current_task->ticks_remaining = 0;
    timer_insert_after(current_task, current_task->sleep_ticks_remaining);

    extern void gic_enable_interrupt(uint32_t int_id);
    gic_enable_interrupt(irq_num);

    sched_reschedule();
}

void sched_wake_irq(uint32_t irq_num) {
    for (uint32_t i = 0; i < task_capacity; i++) {
        if (tasks[i]->state == TASK_STATE_BLOCKED_ON_IRQ && tasks[i]->awaiting_irq == irq_num) {
            uint64_t *tf = sched_task_trap_frame(tasks[i]);
            tf[0] = 1;
            timer_remove(tasks[i]);
            tasks[i]->awaiting_irq = 0;
            tasks[i]->sleep_ticks_remaining = 0;
            sched_make_ready(tasks[i]);
        }
    }
}

void sched_ps_syscall(uint64_t *regs, uint32_t index) {
    if (!regs) return;
    if (index >= task_capacity || tasks[index]->state == TASK_STATE_FREE ||
        tasks[index]->state == TASK_STATE_DEAD) {
        regs[0] = 0;
        return;
    }

    tcb_t *task = tasks[index];
    regs[0] = 1;
    regs[1] = task->tid;
    regs[2] = task->state;
    regs[3] = ((uint64_t)task->priority << 8) | task->base_priority;
    regs[4] = ((uint64_t)task->ticks_remaining << 32) | task->wait_time;
    regs[5] = (task->state == TASK_STATE_BLOCKED_ON_WAIT) ?
              task->wait_target_tid : task->ipc_target_tid;
    regs[6] = ((uint64_t)task->parent_tid << 32) | task->awaiting_irq;
    regs[7] = task->sched_core_id;
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

void sched_debug_info_syscall(uint64_t *regs) {
    if (!regs) {
        return;
    }

    regs[0] = LOG_LEVEL;
    regs[1] = ((uint64_t)sched_online_core_count() << 32) | 1000;
    regs[2] = task_capacity;
    regs[3] = MAX_USER_ASIDS;
    regs[4] = USER_STACK_MAX_SIZE;
    regs[5] = ((uint64_t)MAX_MEM_OBJECTS << 32) | MAX_MEM_MAPPINGS;
    regs[6] = ((uint64_t)memcap_object_count() << 32) | memcap_mapping_count();
    regs[7] = current_task ? (((uint64_t)current_task->vm.count << 32) |
                              current_task->vm.capacity) : 0;
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
        if (tasks[i] == task) {
            return (int)i;
        }
    }
    return -1;
}

void sched_handoff_to_task(tcb_t *target) {
    sched_core_t *core = sched_current_core();
    current_task = core->running_task;
    if (!current_task || !target || target == current_task) {
        return;
    }

    int target_idx = task_index_from_ptr(target);
    if (target_idx < 0) {
        return;
    }

    if (target->sched_core_id != core->core_id) {
        signal_remote_ready(target);
        sched_reschedule();
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
        prev->ticks_remaining = quantum_for_priority(prev->priority);
        prev->quantum = prev->ticks_remaining;
        ready_enqueue_tail(prev);
    }
    ready_remove(target);
    sched_set_current(core, target, target_idx);
    target->state = TASK_STATE_RUNNING;
    target->wait_time = 0;
    target->ticks_remaining = quantum_for_priority(target->priority);
    target->quantum = target->ticks_remaining;
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

    sched_reschedule();
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

    LOG_DEBUG_HEX("SCHED: Killing task. TID = ", tid);
    if (regs) regs[0] = 0;

    if (target == current_task) {
        terminate_current_task(TASK_TERM_KILLED, -1, 0, 0, 0);
    }

    if (target->sched_core_id != current_core_id() &&
        target->sched_core_id < MAX_SCHED_CORES &&
        sched_cores[target->sched_core_id].online) {
        sched_request_remote_kill(target);
        return 0;
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
        LOG_DEBUG_HEX("SCHED: Fault VMA start: ", vma->start);
        LOG_DEBUG_HEX("SCHED: Fault VMA end: ", vma->end);
        LOG_DEBUG_HEX("SCHED: Fault VMA flags: ", vma->flags);
    }

    terminate_current_task(TASK_TERM_FAULTED, -1, esr, elr, fault_va);
}

int sched_resolve_user_page(uint64_t user_va, int write) {
    return sched_current_is_user() ?
           sched_resolve_task_page(current_task, user_va, write) : -1;
}

static int find_free_dma_object(void) {
    for (uint32_t i = 1; i < MAX_DMA_OBJECTS; i++) {
        if (!dma_objects[i].present) {
            return (int)i;
        }
    }
    return -1;
}

void sched_dma_export_syscall(uint64_t *regs, uint64_t user_va, uint64_t size,
                              uint64_t rights) {
    if (!regs || !sched_current_is_user() || !current_task->pgd ||
        user_va == 0 || size == 0 ||
        (user_va & (PAGE_SIZE - 1)) != 0 ||
        (size & (PAGE_SIZE - 1)) != 0 ||
        size > (uint64_t)DMA_OBJECT_MAX_PAGES * PAGE_SIZE ||
        user_va > VMA_USER_LIMIT || size > VMA_USER_LIMIT - user_va) {
        if (regs) regs[0] = (uint64_t)-1;
        return;
    }

    uint32_t page_count = (uint32_t)(size / PAGE_SIZE);
    int object_id = find_free_dma_object();
    if (object_id < 0) {
        regs[0] = (uint64_t)-1;
        return;
    }

    dma_object_t *object = &dma_objects[object_id];
    object->present = 1;
    object->owner_tid = current_task->tid;
    object->user_va = user_va;
    object->size = size;
    object->page_count = page_count;
    object->rights = rights;
    for (uint32_t i = 0; i < DMA_OBJECT_MAX_PAGES; i++) {
        object->pages[i] = 0;
    }

    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t va = user_va + ((uint64_t)i * PAGE_SIZE);
        if (sched_resolve_task_page(current_task, va, 1) < 0) {
            clear_dma_object(object);
            regs[0] = (uint64_t)-1;
            return;
        }

        uint64_t pa = 0;
        uint64_t entry = 0;
        if (vmm_query_page(current_task->pgd, va, &pa, &entry) < 0 ||
            (entry & PTE_USER) == 0 ||
            !frame_from_paddr(pa & ~(PAGE_SIZE - 1)) ||
            frame_ref(pa & ~(PAGE_SIZE - 1)) < 0) {
            clear_dma_object(object);
            regs[0] = (uint64_t)-1;
            return;
        }
        object->pages[i] = pa & ~(PAGE_SIZE - 1);
    }

    int cap = install_cap(current_task, OCAP_DMA, (uint32_t)object_id,
                          OCAP_RIGHT_DMA | OCAP_RIGHT_MAP, 0);
    if (cap < 0) {
        clear_dma_object(object);
        regs[0] = (uint64_t)-1;
        return;
    }

    regs[0] = (uint64_t)cap;
    regs[1] = size;
}

void sched_dma_paddr_syscall(uint64_t *regs, uint32_t dma_cap, uint64_t offset) {
    ocap_t cap;
    if (!regs || !current_task ||
        ocap_lookup(&current_task->caps, dma_cap, OCAP_DMA,
                    OCAP_RIGHT_DMA | OCAP_RIGHT_MAP, &cap) < 0 ||
        cap.object_id == 0 || cap.object_id >= MAX_DMA_OBJECTS ||
        !dma_objects[cap.object_id].present ||
        dma_objects[cap.object_id].owner_tid != current_task->tid) {
        if (regs) regs[0] = 0;
        return;
    }

    dma_object_t *object = &dma_objects[cap.object_id];
    if (offset >= object->size) {
        regs[0] = 0;
        return;
    }

    uint32_t page = (uint32_t)(offset / PAGE_SIZE);
    uint64_t page_off = offset & (PAGE_SIZE - 1);
    if (page >= object->page_count || !object->pages[page]) {
        regs[0] = 0;
        return;
    }

    regs[0] = object->pages[page] + page_off;
    regs[1] = object->size - offset;
}

void sched_dma_release_syscall(uint64_t *regs, uint32_t dma_cap) {
    if (!regs || !current_task) {
        return;
    }

    ocap_t cap;
    if (ocap_lookup(&current_task->caps, dma_cap, OCAP_DMA,
                    OCAP_RIGHT_DMA, &cap) < 0) {
        regs[0] = (uint64_t)-1;
        return;
    }

    release_dma_object(cap.object_id);
    ocap_revoke_slot(&current_task->caps, dma_cap);
    regs[0] = 0;
}

void sched_exit_syscall(uint64_t *regs, int exit_code) {
    (void)regs;
    if (!current_task) return;

    LOG_DEBUG_HEX("SCHED: Task exited. TID = ", current_task->tid);
    LOG_DEBUG_HEX("SCHED: Exit code: ", exit_code);

    terminate_current_task(TASK_TERM_EXITED, exit_code, 0, 0, 0);
}

