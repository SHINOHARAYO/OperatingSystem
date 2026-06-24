#pragma once

#include <stdint.h>

typedef enum {
    PLATFORM_TIMER_VIRTUAL = 0,
    PLATFORM_TIMER_PHYSICAL = 1,
} platform_timer_kind_t;

typedef struct {
    const char *name;
    uint64_t uart_pa;
    uint64_t gicd_pa;
    uint64_t gicc_pa;
    uint64_t device_pa;
    uint64_t device_size;
    uint64_t bootstrap_ram_limit;
    uint32_t timer_irq;
    platform_timer_kind_t timer_kind;
    uint8_t supports_smp;
    uint8_t supports_virtio;
} platform_info_t;

void platform_early_init(void);
int platform_validate_acpi(void);
const platform_info_t *platform_get(void);
int platform_is_pi4(void);

