#include "platform.h"
#include "acpi.h"
#include "log.h"

#ifndef NEPTUNE_PLATFORM_PI4
#define NEPTUNE_PLATFORM_PI4 0
#endif

static platform_info_t platform;

void platform_early_init(void) {
#if NEPTUNE_PLATFORM_PI4
    platform.name = "Raspberry Pi 4";
    platform.uart_pa = 0xFE201000ULL;
    platform.gicd_pa = 0xFF841000ULL;
    platform.gicc_pa = 0xFF842000ULL;
    platform.device_pa = 0xFE000000ULL;
    platform.device_size = 0x02000000ULL;
    platform.bootstrap_ram_limit = 8ULL * 1024 * 1024 * 1024;
    platform.timer_irq = 30;
    platform.timer_kind = PLATFORM_TIMER_PHYSICAL;
    platform.supports_smp = 0;
    platform.supports_virtio = 0;
#else
    platform.name = "QEMU virt";
    platform.uart_pa = 0x09000000ULL;
    platform.gicd_pa = 0;
    platform.gicc_pa = 0;
    platform.device_pa = 0;
    platform.device_size = 0;
    platform.bootstrap_ram_limit = 0;
    platform.timer_irq = 27;
    platform.timer_kind = PLATFORM_TIMER_VIRTUAL;
    platform.supports_smp = 1;
    platform.supports_virtio = 1;
#endif
}

const platform_info_t *platform_get(void) {
    return &platform;
}

int platform_is_pi4(void) {
#if NEPTUNE_PLATFORM_PI4
    return 1;
#else
    return 0;
#endif
}

int platform_validate_acpi(void) {
    if (!platform_is_pi4()) {
        return 0;
    }

    uint64_t gicd = acpi_get_gicd_base();
    uint64_t gicc = acpi_get_gicc_base();
    if (gicd != platform.gicd_pa || gicc != platform.gicc_pa) {
        LOG_FAIL("PLATFORM: Pi 4 ACPI GIC layout is missing or unsupported.");
        LOG_DEBUG_HEX("PLATFORM: MADT GICD: ", gicd);
        LOG_DEBUG_HEX("PLATFORM: MADT GICC: ", gicc);
        return -1;
    }

    LOG_OK("PLATFORM: Raspberry Pi 4 ACPI hardware validated.");
    return 0;
}
