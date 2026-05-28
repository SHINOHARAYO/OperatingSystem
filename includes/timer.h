#pragma once

#include <stdint.h>
#include <stddef.h>

void timer_init(void);
void timer_init_secondary(void);

void timer_handle_interrupt(void);

uint64_t timer_get_uptime_seconds(void);

uint64_t timer_get_uptime_ms(void);

uint64_t timer_get_uptime_ns(void);
