#pragma once

#include <stdint.h>

void smp_init(void);
void smp_send_reschedule(uint32_t core_id);
void smp_secondary_main(uint64_t core_id) __attribute__((noreturn));
