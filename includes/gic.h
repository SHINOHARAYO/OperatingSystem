#pragma once

#include <stdint.h>
#include <stddef.h>

void gic_init(void);

void gic_enable_interrupt(uint32_t int_id);

void gic_disable_interrupt(uint32_t int_id);

uint32_t gic_acknowledge_interrupt(void);

void gic_end_of_interrupt(uint32_t int_id);
