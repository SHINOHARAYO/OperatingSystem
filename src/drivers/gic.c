#include "gic.h"
#include "log.h"
#include "uart.h"
#include "acpi.h"
#include "platform.h"

static uint64_t GICD_BASE = 0;
static uint64_t GICC_BASE = 0;
static uint64_t GICR_BASE = 0;
static uint32_t GICR_LENGTH = 0;
static int gic_version = 0;

#define mmio_read32(addr) (*((volatile uint32_t *)(addr)))
#define mmio_write32(addr, val) (*((volatile uint32_t *)(addr)) = (val))
#define mmio_read64(addr) (*((volatile uint64_t *)(addr)))
#define mmio_write64(addr, val) (*((volatile uint64_t *)(addr)) = (val))

#define GICD_CTLR        (GICD_BASE + 0x000)
#define GICD_TYPER       (GICD_BASE + 0x004)
#define GICD_IGROUPR(n)  (GICD_BASE + 0x080 + ((n) * 4))
#define GICD_ISENABLER(n)(GICD_BASE + 0x100 + ((n) * 4))
#define GICD_ICENABLER(n)(GICD_BASE + 0x180 + ((n) * 4))
#define GICD_ISPENDR(n)  (GICD_BASE + 0x200 + ((n) * 4))
#define GICD_ICPENDR(n)  (GICD_BASE + 0x280 + ((n) * 4))
#define GICD_IPRIORITYR(n) (GICD_BASE + 0x400 + ((n) * 4))
#define GICD_ITARGETSR(n)  (GICD_BASE + 0x800 + ((n) * 4))
#define GICD_ICFGR(n)    (GICD_BASE + 0xC00 + ((n) * 4))
#define GICD_SGIR        (GICD_BASE + 0xF00)
#define GICD_IROUTER(n)  (GICD_BASE + 0x6000 + ((uint64_t)(n) * 8))

#define GICC_CTLR        (GICC_BASE + 0x0000)
#define GICC_PMR         (GICC_BASE + 0x0004)
#define GICC_BPR         (GICC_BASE + 0x0008)
#define GICC_IAR         (GICC_BASE + 0x000C)
#define GICC_EOIR        (GICC_BASE + 0x0010)
#define GICC_RPR         (GICC_BASE + 0x0014)
#define GICC_HPPIR       (GICC_BASE + 0x0018)

#define GICD_CTLR_ENABLE_GRP1_NS (1U << 1)
#define GICD_CTLR_ARE_NS         (1U << 4)
#define GICD_CTLR_RWP            (1U << 31)

#define GICR_STRIDE              0x20000ULL
#define GICR_SGI_BASE(rd)        ((rd) + 0x10000ULL)
#define GICR_CTLR(rd)            ((rd) + 0x0000)
#define GICR_TYPER(rd)           ((rd) + 0x0008)
#define GICR_WAKER(rd)           ((rd) + 0x0014)
#define GICR_IGROUPR0(rd)        (GICR_SGI_BASE(rd) + 0x0080)
#define GICR_ISENABLER0(rd)      (GICR_SGI_BASE(rd) + 0x0100)
#define GICR_ICENABLER0(rd)      (GICR_SGI_BASE(rd) + 0x0180)
#define GICR_ICPENDR0(rd)        (GICR_SGI_BASE(rd) + 0x0280)
#define GICR_IPRIORITYR(rd, n)   (GICR_SGI_BASE(rd) + 0x0400 + ((n) * 4))
#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

static uint32_t current_core_id(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static uint32_t mpidr_affinity_value(uint64_t mpidr) {
    return (uint32_t)((mpidr & 0xFF) |
                      (((mpidr >> 8) & 0xFF) << 8) |
                      (((mpidr >> 16) & 0xFF) << 16) |
                      (((mpidr >> 32) & 0xFF) << 24));
}

static uint32_t current_mpidr_affinity(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr_affinity_value(mpidr);
}

static void gicd_wait_rwp(void) {
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if ((mmio_read32(GICD_CTLR) & GICD_CTLR_RWP) == 0) {
            return;
        }
        __asm__ volatile("yield");
    }
}

static uint64_t gicv3_find_redistributor(void) {
    if (!GICR_BASE) {
        return 0;
    }

    uint32_t self_affinity = current_mpidr_affinity();
    uint64_t length = GICR_LENGTH ? GICR_LENGTH : (GICR_STRIDE * 8);
    for (uint64_t offset = 0; offset + GICR_STRIDE <= length; offset += GICR_STRIDE) {
        uint64_t rd = GICR_BASE + offset;
        uint64_t typer = mmio_read64(GICR_TYPER(rd));
        uint32_t affinity = (uint32_t)(typer >> 32);
        if (affinity == self_affinity) {
            return rd;
        }
    }

    return GICR_BASE + ((uint64_t)current_core_id() * GICR_STRIDE);
}

static void gicv3_init_redistributor(uint64_t rd) {
    if (!rd) {
        return;
    }

    uint32_t waker = mmio_read32(GICR_WAKER(rd));
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    mmio_write32(GICR_WAKER(rd), waker);
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if ((mmio_read32(GICR_WAKER(rd)) & GICR_WAKER_CHILDREN_ASLEEP) == 0) {
            break;
        }
        __asm__ volatile("yield");
    }

    mmio_write32(GICR_ICENABLER0(rd), 0xFFFFFFFF);
    mmio_write32(GICR_ICPENDR0(rd), 0xFFFFFFFF);
    mmio_write32(GICR_IGROUPR0(rd), 0xFFFFFFFF);
    for (uint32_t i = 0; i < 8; i++) {
        mmio_write32(GICR_IPRIORITYR(rd, i), 0xA0A0A0A0);
    }
    mmio_write32(GICR_ISENABLER0(rd), (1U << 1));
}

static int gicv3_enable_sre(void) {
    uint64_t sre = 0;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
    sre |= 0x7;
    __asm__ volatile("msr ICC_SRE_EL1, %0" : : "r"(sre) : "memory");
    __asm__ volatile("isb" : : : "memory");
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
    return (sre & 1U) ? 0 : -1;
}

static void gicv3_init_cpu_interface(void) {
    uint64_t rd = gicv3_find_redistributor();
    gicv3_init_redistributor(rd);

    if (gicv3_enable_sre() < 0) {
        LOG_FAIL("GIC: GICv3 system register interface unavailable.");
        return;
    }

    __asm__ volatile("msr ICC_PMR_EL1, %0" : : "r"(0xF0ULL) : "memory");
    __asm__ volatile("msr ICC_BPR1_EL1, %0" : : "r"(0ULL) : "memory");
    __asm__ volatile("msr ICC_CTLR_EL1, %0" : : "r"(0ULL) : "memory");
    __asm__ volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(1ULL) : "memory");
    __asm__ volatile("isb" : : : "memory");
}

static void gicv2_init(void) {
    LOG_DEBUG("GIC: Initializing Generic Interrupt Controller v2.");

    mmio_write32(GICD_CTLR, 0);
    uint32_t typer = mmio_read32(GICD_TYPER);
    uint32_t max_lines = ((typer & 0x1F) + 1) * 32;
    LOG_DEBUG_HEX("GIC: Maximum Interrupt Lines: ", max_lines);
    for (uint32_t i = 1; i < (max_lines / 32); i++) {
        mmio_write32(GICD_ICENABLER(i), 0xFFFFFFFF);
    }
    for (uint32_t i = 1; i < (max_lines / 32); i++) {
        mmio_write32(GICD_ICPENDR(i), 0xFFFFFFFF);
    }

    // Set all interrupt priorities to lowest (0xA0) to allow higher priority ones to preempt
    for (uint32_t i = 8; i < (max_lines / 4); i++) {
        mmio_write32(GICD_IPRIORITYR(i), 0xA0A0A0A0);
    }
    for (uint32_t i = 8; i < (max_lines / 4); i++) {
        mmio_write32(GICD_ITARGETSR(i), 0x01010101);
    }
    mmio_write32(GICD_CTLR, 1);
    LOG_OK("GIC: Distributor (GICD) Enabled.");
    gic_init_cpu_interface();
    
    LOG_OK("GIC: CPU Interface (GICC) Enabled.");
}

static void gicv3_init(void) {
    LOG_DEBUG("GIC: Initializing Generic Interrupt Controller v3.");

    mmio_write32(GICD_CTLR, 0);
    gicd_wait_rwp();

    uint32_t typer = mmio_read32(GICD_TYPER);
    uint32_t max_lines = ((typer & 0x1F) + 1) * 32;
    uint32_t boot_affinity = current_mpidr_affinity();
    LOG_DEBUG_HEX("GIC: Maximum Interrupt Lines: ", max_lines);

    for (uint32_t i = 1; i < (max_lines / 32); i++) {
        mmio_write32(GICD_ICENABLER(i), 0xFFFFFFFF);
        mmio_write32(GICD_ICPENDR(i), 0xFFFFFFFF);
        mmio_write32(GICD_IGROUPR(i), 0xFFFFFFFF);
    }
    for (uint32_t i = 8; i < (max_lines / 4); i++) {
        mmio_write32(GICD_IPRIORITYR(i), 0xA0A0A0A0);
    }
    for (uint32_t int_id = 32; int_id < max_lines; int_id++) {
        mmio_write64(GICD_IROUTER(int_id), boot_affinity);
    }
    gicd_wait_rwp();

    mmio_write32(GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_GRP1_NS);
    gicd_wait_rwp();
    LOG_OK("GIC: Distributor (GICD) Enabled.");

    gic_init_cpu_interface();
    LOG_OK("GIC: CPU Interface (ICC) Enabled.");
}

void gic_init(void) {
    GICD_BASE = acpi_get_gicd_base();
    GICC_BASE = acpi_get_gicc_base();
    GICR_BASE = acpi_get_gicr_base();
    GICR_LENGTH = acpi_get_gicr_length();

    if (!GICD_BASE) GICD_BASE = platform_get()->gicd_pa;
    if (!GICC_BASE) GICC_BASE = platform_get()->gicc_pa;

    if (!GICD_BASE) {
        LOG_FAIL("GIC: Cannot initialize. Distributor base missing from ACPI.");
        return;
    }

    if (GICC_BASE) {
        gic_version = 2;
        gicv2_init();
    } else if (GICR_BASE) {
        gic_version = 3;
        gicv3_init();
    } else {
        LOG_FAIL("GIC: Cannot initialize. CPU interface missing from ACPI.");
    }
}

void gic_init_cpu_interface(void) {
    if (gic_version == 3) {
        gicv3_init_cpu_interface();
        return;
    }

    if (!GICC_BASE) {
        GICC_BASE = acpi_get_gicc_base();
    }
    if (!GICC_BASE) {
        return;
    }

    mmio_write32(GICC_CTLR, 0);
    mmio_write32(GICC_PMR, 0xF0);
    mmio_write32(GICC_CTLR, 1);
}

void gic_enable_interrupt(uint32_t int_id) {
    if (gic_version == 3 && int_id < 32) {
        uint64_t rd = gicv3_find_redistributor();
        if (!rd) {
            return;
        }
        mmio_write32(GICR_ISENABLER0(rd), 1U << int_id);
        return;
    }

    uint32_t reg_idx = int_id / 32;
    uint32_t bit_idx = int_id % 32;
    uint32_t val = mmio_read32(GICD_ISENABLER(reg_idx));
    val |= (1 << bit_idx);
    mmio_write32(GICD_ISENABLER(reg_idx), val);
}

void gic_disable_interrupt(uint32_t int_id) {
    if (gic_version == 3 && int_id < 32) {
        uint64_t rd = gicv3_find_redistributor();
        if (!rd) {
            return;
        }
        mmio_write32(GICR_ICENABLER0(rd), 1U << int_id);
        return;
    }

    uint32_t reg_idx = int_id / 32;
    uint32_t bit_idx = int_id % 32;
    
    uint32_t val = mmio_read32(GICD_ICENABLER(reg_idx));
    val |= (1 << bit_idx);
    mmio_write32(GICD_ICENABLER(reg_idx), val);
}

uint32_t gic_acknowledge_interrupt(void) {
    if (gic_version == 3) {
        uint64_t iar = 0;
        __asm__ volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
        return (uint32_t)(iar & 0xFFFFFFU);
    }

    // This tells the GIC we are handling this interrupt, changing its state to Active.
    return mmio_read32(GICC_IAR) & 0x3FF;
}

void gic_end_of_interrupt(uint32_t int_id) {
    if (gic_version == 3) {
        __asm__ volatile("msr ICC_EOIR1_EL1, %0" : : "r"((uint64_t)int_id) : "memory");
        return;
    }

    // This tells the GIC we are done, dropping the priority and clearing the Active state.
    mmio_write32(GICC_EOIR, int_id);
}

void gic_send_sgi(uint32_t int_id, uint32_t target_cpu_mask) {
    if (!GICD_BASE || int_id >= 16 || target_cpu_mask == 0) {
        return;
    }
    if (gic_version == 3) {
        uint64_t target_list = target_cpu_mask & 0xFFFFU;
        uint64_t sgi = ((uint64_t)(int_id & 0xF) << 24) | target_list;
        __asm__ volatile("msr ICC_SGI1R_EL1, %0" : : "r"(sgi) : "memory");
        __asm__ volatile("isb" : : : "memory");
        return;
    }

    uint32_t value = ((target_cpu_mask & 0xFF) << 16) | (int_id & 0xF);
    mmio_write32(GICD_SGIR, value);
}
