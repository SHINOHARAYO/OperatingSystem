#include "smp.h"
#include "sched.h"
#include "acpi.h"
#include "gic.h"
#include "log.h"
#include "mmu.h"
#include "timer.h"

#define PSCI_CPU_ON 0xC4000003ULL
#define SMP_STACK_SIZE 4096
#define SMP_IPI_RESCHEDULE 1
#define SMP_IPI_TLB_SHOOTDOWN 2
#define SMP_TLB_VA_ASID 1
#define SMP_TLB_VA_ALL 2
#define SMP_TLB_ASID 3
#define SMP_TLB_ALL 4
#define SMP_ASID_COUNT 65536

extern uint64_t l1_table[512];
extern uint64_t high_l1_table[512];
extern uint8_t vector_table_el1[];
extern void smp_secondary_entry(void);

uint8_t smp_secondary_stacks[MAX_SCHED_CORES][SMP_STACK_SIZE]
    __attribute__((aligned(4096)));

typedef struct {
    volatile uint32_t seq;
    volatile uint32_t ack;
    volatile uint32_t type;
    volatile uint32_t asid;
    volatile uint64_t va;
} smp_tlb_request_t;

static volatile uint32_t smp_online_mask = 1;
static volatile uint32_t smp_tlb_seq = 1;
static smp_tlb_request_t smp_tlb_requests[MAX_SCHED_CORES];
static volatile uint32_t smp_asid_masks[SMP_ASID_COUNT];

static uint32_t smp_current_core_id(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static void smp_local_tlb_flush_va_asid(uint64_t va, uint16_t asid) {
    uint64_t op = ((uint64_t)asid << 48) | ((va >> 12) & 0x00000FFFFFFFFFFFULL);
    __asm__ volatile("tlbi vae1is, %0" : : "r"(op) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
}

static void smp_local_tlb_flush_va_all_asids(uint64_t va) {
    __asm__ volatile("tlbi vaae1is, %0" : : "r"(va >> 12) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
}

static void smp_local_tlb_flush_asid(uint16_t asid) {
    uint64_t op = (uint64_t)asid << 48;
    __asm__ volatile("tlbi aside1is, %0" : : "r"(op) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
}

static void smp_local_tlb_flush_all(void) {
    __asm__ volatile("tlbi vmalle1is\n dsb ish\n isb" : : : "memory");
}

static void smp_process_tlb_request(uint32_t core_id) {
    if (core_id >= MAX_SCHED_CORES) {
        return;
    }

    smp_tlb_request_t *request = &smp_tlb_requests[core_id];
    uint32_t seq = __atomic_load_n(&request->seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || __atomic_load_n(&request->ack, __ATOMIC_ACQUIRE) == seq) {
        return;
    }

    if (request->type == SMP_TLB_VA_ASID) {
        smp_local_tlb_flush_va_asid(request->va, (uint16_t)request->asid);
    } else if (request->type == SMP_TLB_VA_ALL) {
        smp_local_tlb_flush_va_all_asids(request->va);
    } else if (request->type == SMP_TLB_ASID) {
        smp_local_tlb_flush_asid((uint16_t)request->asid);
    } else if (request->type == SMP_TLB_ALL) {
        smp_local_tlb_flush_all();
    }

    __atomic_store_n(&request->ack, seq, __ATOMIC_RELEASE);
}

static void smp_tlb_broadcast(uint32_t type, uint64_t va, uint16_t asid,
                              uint32_t requested_targets) {
    uint32_t self = smp_current_core_id();
    uint32_t online = __atomic_load_n(&smp_online_mask, __ATOMIC_ACQUIRE);
    uint32_t seq = __atomic_add_fetch(&smp_tlb_seq, 1, __ATOMIC_ACQ_REL);
    uint32_t targets = 0;

    for (uint32_t core_id = 0; core_id < MAX_SCHED_CORES; core_id++) {
        uint32_t bit = 1U << core_id;
        if (core_id == self || (online & bit) == 0 ||
            (requested_targets & bit) == 0) {
            continue;
        }

        smp_tlb_requests[core_id].type = type;
        smp_tlb_requests[core_id].va = va;
        smp_tlb_requests[core_id].asid = asid;
        __atomic_store_n(&smp_tlb_requests[core_id].seq, seq, __ATOMIC_RELEASE);
        targets |= 1U << core_id;
    }

    if (targets == 0) {
        return;
    }

    gic_send_sgi(SMP_IPI_TLB_SHOOTDOWN, targets);

    for (uint32_t spin = 0; spin < 1000000; spin++) {
        smp_process_tlb_request(self);
        uint32_t done = 1;
        for (uint32_t core_id = 0; core_id < MAX_SCHED_CORES; core_id++) {
            if ((targets & (1U << core_id)) == 0) {
                continue;
            }
            if (__atomic_load_n(&smp_tlb_requests[core_id].ack,
                                __ATOMIC_ACQUIRE) != seq) {
                done = 0;
                break;
            }
        }
        if (done) {
            return;
        }
        __asm__ volatile("yield");
    }

    LOG_WARN("SMP: TLB shootdown ack timeout.");
}

void smp_asid_activate(uint16_t asid) {
    if (asid == 0) {
        return;
    }
    uint32_t core_id = smp_current_core_id();
    if (core_id < MAX_SCHED_CORES) {
        __atomic_fetch_or(&smp_asid_masks[asid], 1U << core_id, __ATOMIC_RELEASE);
    }
}

void smp_asid_forget(uint16_t asid) {
    if (asid != 0) {
        __atomic_store_n(&smp_asid_masks[asid], 0, __ATOMIC_RELEASE);
    }
}

void smp_tlb_shootdown_va_asid(uint64_t va, uint16_t asid) {
    uint32_t targets = __atomic_load_n(&smp_asid_masks[asid], __ATOMIC_ACQUIRE);
    uint32_t self = smp_current_core_id();
    if (self < MAX_SCHED_CORES && (targets & (1U << self)) != 0) {
        smp_local_tlb_flush_va_asid(va, asid);
    }
    smp_tlb_broadcast(SMP_TLB_VA_ASID, va, asid, targets);
}

void smp_tlb_shootdown_va_all_asids(uint64_t va) {
    smp_local_tlb_flush_va_all_asids(va);
    smp_tlb_broadcast(SMP_TLB_VA_ALL, va, 0, UINT32_MAX);
}

void smp_tlb_shootdown_asid(uint16_t asid) {
    uint32_t targets = __atomic_load_n(&smp_asid_masks[asid], __ATOMIC_ACQUIRE);
    uint32_t self = smp_current_core_id();
    if (self < MAX_SCHED_CORES && (targets & (1U << self)) != 0) {
        smp_local_tlb_flush_asid(asid);
    }
    smp_tlb_broadcast(SMP_TLB_ASID, 0, asid, targets);
}

void smp_tlb_shootdown_all(void) {
    smp_local_tlb_flush_all();
    smp_tlb_broadcast(SMP_TLB_ALL, 0, 0, UINT32_MAX);
}

void smp_handle_tlb_shootdown_ipi(void) {
    smp_process_tlb_request(smp_current_core_id());
}

void smp_send_reschedule(uint32_t core_id) {
    if (core_id >= MAX_SCHED_CORES) {
        return;
    }
    gic_send_sgi(SMP_IPI_RESCHEDULE, 1U << core_id);
}

static uint64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context) {
    register uint64_t x0 asm("x0") = PSCI_CPU_ON;
    register uint64_t x1 asm("x1") = mpidr;
    register uint64_t x2 asm("x2") = entry;
    register uint64_t x3 asm("x3") = context;

    __asm__ volatile("hvc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

static void smp_enable_mmu(void) {
    uint64_t mair = (0xFFULL << (8 * MT_NORMAL)) |
                    (0x00ULL << (8 * MT_DEVICE_nGnRnE));
    uint64_t tcr = (25ULL << 0) |
                   (1ULL << 8) |
                   (1ULL << 10) |
                   (3ULL << 12) |
                   (25ULL << 16) |
                   (1ULL << 24) |
                   (1ULL << 26) |
                   (3ULL << 28) |
                   (2ULL << 30) |
                   (2ULL << 32);

    __asm__ volatile("msr vbar_el1, %0" : : "r"(vector_table_el1) : "memory");
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair) : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)l1_table) : "memory");
    __asm__ volatile("msr ttbr1_el1, %0" : : "r"((uint64_t)high_l1_table) : "memory");
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr) : "memory");
    __asm__ volatile("dsb ish\n tlbi vmalle1\n dsb ish\n isb" : : : "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile("dsb ish\n isb" : : : "memory");
    __asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
    __asm__ volatile("isb" : : : "memory");
}

void smp_secondary_main(uint64_t core_id) {
    smp_enable_mmu();
    gic_init_cpu_interface();
    sched_secondary_core_online((uint32_t)core_id);
    timer_init_secondary();
    __asm__ volatile("msr daifclr, #2" : : : "memory");
    if (core_id < MAX_SCHED_CORES) {
        __atomic_fetch_or(&smp_online_mask, 1U << core_id, __ATOMIC_RELEASE);
    }

    while (1) {
        __asm__ volatile("wfi");
    }
}

void smp_init(void) {
    uint32_t count = acpi_get_cpu_count();
    if (count > MAX_SCHED_CORES) {
        count = MAX_SCHED_CORES;
    }
    if (count <= 1) {
        LOG_OK("SMP: Boot core only.");
        return;
    }

    uint64_t boot_mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(boot_mpidr));
    boot_mpidr &= 0xFF00FFFFFFULL;

    uint32_t started = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t mpidr = acpi_get_cpu_mpidr(i) & 0xFF00FFFFFFULL;
        if (mpidr == 0 || mpidr == boot_mpidr) {
            continue;
        }

        uint32_t core_id = (uint32_t)(mpidr & 0xFF);
        if (core_id == 0 || core_id >= MAX_SCHED_CORES) {
            continue;
        }

        uint64_t rc = psci_cpu_on(mpidr, (uint64_t)smp_secondary_entry, core_id);
        if (rc == 0) {
            started++;
        } else {
            LOG_WARN("SMP: PSCI CPU_ON failed.");
            LOG_DEBUG_HEX("SMP: PSCI status: ", rc);
        }
    }

    for (uint64_t spin = 0; spin < 10000000; spin++) {
        if (sched_online_core_count() >= started) {
            break;
        }
        __asm__ volatile("yield");
    }

    LOG_OK_HEX("SMP: Online cores: ", sched_online_core_count());
}
