#include "gic.h"
#include "log.h"
#include "uart.h"
#include "acpi.h"

static uint64_t GICD_BASE = 0;
static uint64_t GICC_BASE = 0;

#define mmio_read32(addr) (*((volatile uint32_t *)(addr)))
#define mmio_write32(addr, val) (*((volatile uint32_t *)(addr)) = (val))

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

#define GICC_CTLR        (GICC_BASE + 0x0000)
#define GICC_PMR         (GICC_BASE + 0x0004)
#define GICC_BPR         (GICC_BASE + 0x0008)
#define GICC_IAR         (GICC_BASE + 0x000C)
#define GICC_EOIR        (GICC_BASE + 0x0010)
#define GICC_RPR         (GICC_BASE + 0x0014)
#define GICC_HPPIR       (GICC_BASE + 0x0018)

void gic_init(void) {
    LOG_DEBUG("GIC: Initializing Generic Interrupt Controller v2.");

    GICD_BASE = acpi_get_gicd_base();
    GICC_BASE = acpi_get_gicc_base();
    if (!GICD_BASE || !GICC_BASE) {
        LOG_FAIL("GIC: Cannot initialize. Base addresses missing from ACPI.");
        return;
    }
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
    mmio_write32(GICC_CTLR, 0);

    // Set Priority Mask to allow all interrupts (lowest priority is 0xFF, so we want to be able to receive them)
    mmio_write32(GICC_PMR, 0xF0);
    // Bit 0: EnableGrp0 -> Enable Group 0 interrupts (Secure)
    mmio_write32(GICC_CTLR, 1);
    
    LOG_OK("GIC: CPU Interface (GICC) Enabled.");
}

void gic_enable_interrupt(uint32_t int_id) {
    uint32_t reg_idx = int_id / 32;
    uint32_t bit_idx = int_id % 32;
    uint32_t val = mmio_read32(GICD_ISENABLER(reg_idx));
    val |= (1 << bit_idx);
    mmio_write32(GICD_ISENABLER(reg_idx), val);
}

void gic_disable_interrupt(uint32_t int_id) {
    uint32_t reg_idx = int_id / 32;
    uint32_t bit_idx = int_id % 32;
    
    uint32_t val = mmio_read32(GICD_ICENABLER(reg_idx));
    val |= (1 << bit_idx);
    mmio_write32(GICD_ICENABLER(reg_idx), val);
}

uint32_t gic_acknowledge_interrupt(void) {
    // This tells the GIC we are handling this interrupt, changing its state to Active.
    return mmio_read32(GICC_IAR) & 0x3FF;
}

void gic_end_of_interrupt(uint32_t int_id) {
    // This tells the GIC we are done, dropping the priority and clearing the Active state.
    mmio_write32(GICC_EOIR, int_id);
}
