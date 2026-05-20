#include "timer.h"
#include "gic.h"
#include "log.h"
#include "uart.h"
#include "sched.h"

// The ARM generic timer uses the following system registers:
// CNTFRQ_EL0 - Timer Frequency
// CNTP_TVAL_EL0 - Timer Value (down counter)
// CNTP_CTL_EL0 - Timer Control
// CNTPCT_EL0 - Physical Count

// The physical timer is typically routed to PPI 30 on the GIC
#define TIMER_IRQ 30

static uint64_t timer_freq;
static uint64_t ticks = 0;

void timer_init(void) {
    LOG_INFO("TIMER: Initializing ARM Generic Timer...");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_freq));
    LOG_INFO_HEX("TIMER: Frequency (Hz): ", timer_freq);

    if (timer_freq == 0) {
        LOG_FAIL("TIMER PANIC: Timer frequency is zero!");
        while(1);
    }
    timer_freq = timer_freq / 10;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_freq));
    // CNTP_CTL_EL0: Bit 0 = Enable, Bit 1 = IMASK (0 to unmask)
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ULL));
    gic_enable_interrupt(TIMER_IRQ);

    LOG_OK("TIMER: Generic Timer Enabled and Interrupt unmasked.");
}

void timer_handle_interrupt(void) {
    ticks++;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_freq));
}
uint64_t timer_get_uptime_seconds(void) {
    return ticks / 10;
}
