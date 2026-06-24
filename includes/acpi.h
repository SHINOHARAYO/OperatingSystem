#pragma once

#include <stdint.h>
#include <stddef.h>

int acpi_init(void *rsdp_ptr);

uint64_t acpi_get_gicd_base(void);

uint64_t acpi_get_gicc_base(void);

uint64_t acpi_get_gicr_base(void);

uint32_t acpi_get_gicr_length(void);

uint32_t acpi_get_cpu_count(void);

uint64_t acpi_get_cpu_mpidr(uint32_t index);

typedef struct {
    uint8_t present;
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint64_t bar_pa;
    uint64_t bar_size;
    uint32_t common_off;
    uint32_t notify_off;
    uint32_t isr_off;
    uint32_t device_off;
    uint32_t notify_multiplier;
} acpi_virtio_pci_info_t;

typedef acpi_virtio_pci_info_t acpi_virtio_blk_info_t;

int acpi_get_virtio_blk_info(acpi_virtio_blk_info_t *out);
int acpi_get_virtio_gpu_info(acpi_virtio_pci_info_t *out);
