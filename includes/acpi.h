#pragma once

#include <stdint.h>
#include <stddef.h>

int acpi_init(void *rsdp_ptr);

uint64_t acpi_get_gicd_base(void);

uint64_t acpi_get_gicc_base(void);
