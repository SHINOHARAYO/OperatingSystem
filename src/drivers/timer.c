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

static uint64_t timer_freq_hz;
static uint64_t timer_period_counts;
static uint64_t timer_base_count;
static uint64_t ticks = 0;

void timer_init(void) {
    LOG_DEBUG("TIMER: Initializing ARM Generic Timer...");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_freq_hz));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(timer_base_count));
    LOG_DEBUG_HEX("TIMER: Frequency (Hz): ", timer_freq_hz);

    if (timer_freq_hz == 0) {
        LOG_FAIL("TIMER PANIC: Timer frequency is zero!");
        while(1);
    }
    timer_period_counts = timer_freq_hz / 1000;
    if (timer_period_counts == 0) {
        timer_period_counts = 1;
    }
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_period_counts));
    // CNTP_CTL_EL0: Bit 0 = Enable, Bit 1 = IMASK (0 to unmask)
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ULL));
    gic_enable_interrupt(TIMER_IRQ);

    LOG_OK("TIMER: Generic Timer Enabled at 1 kHz.");
}

void timer_handle_interrupt(void) {
    ticks++;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_period_counts));
}
uint64_t timer_get_uptime_seconds(void) {
    return ticks / 1000;
}

uint64_t timer_get_uptime_ms(void) {
    return ticks;
}

uint64_t timer_get_uptime_ns(void) {
    uint64_t count = 0;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(count));
    return ((count - timer_base_count) * 1000000000ULL) / timer_freq_hz;
}
