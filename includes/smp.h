#pragma once

#include <stdint.h>

void smp_init(void);
void smp_send_reschedule(uint32_t core_id);
void smp_asid_activate(uint16_t asid);
void smp_asid_forget(uint16_t asid);
void smp_tlb_shootdown_va_asid(uint64_t va, uint16_t asid);
void smp_tlb_shootdown_va_all_asids(uint64_t va);
void smp_tlb_shootdown_asid(uint16_t asid);
void smp_tlb_shootdown_all(void);
void smp_handle_tlb_shootdown_ipi(void);
void smp_secondary_main(uint64_t core_id) __attribute__((noreturn));
