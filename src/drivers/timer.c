#include "timer.h"
#include "gic.h"
#include "log.h"
#include "uart.h"
#include "sched.h"
#include "platform.h"

// The ARM generic virtual timer uses the following system registers:
// CNTFRQ_EL0 - Timer Frequency
// CNTV_TVAL_EL0 - Timer Value (down counter)
// CNTV_CTL_EL0 - Timer Control
// CNTVCT_EL0 - Virtual Count

static uint64_t timer_freq_hz;
static uint64_t timer_period_counts;
static uint64_t timer_base_count;
static uint64_t ticks = 0;

static uint32_t timer_current_core_id(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static void timer_program_local(void) {
    if (timer_period_counts == 0) {
        uint64_t freq = 0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        timer_period_counts = freq / 1000;
        if (timer_period_counts == 0) {
            timer_period_counts = 1;
        }
    }

    if (platform_get()->timer_kind == PLATFORM_TIMER_PHYSICAL) {
        __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_period_counts));
        __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ULL));
    } else {
        __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_period_counts));
        __asm__ volatile("msr cntv_ctl_el0, %0" : : "r"(1ULL));
    }
    gic_enable_interrupt(platform_get()->timer_irq);
}

void timer_init(void) {
    LOG_DEBUG("TIMER: Initializing ARM Generic Timer...");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_freq_hz));
    if (platform_get()->timer_kind == PLATFORM_TIMER_PHYSICAL) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(timer_base_count));
    } else {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(timer_base_count));
    }
    LOG_DEBUG_HEX("TIMER: Frequency (Hz): ", timer_freq_hz);

    if (timer_freq_hz == 0) {
        LOG_FAIL("TIMER PANIC: Timer frequency is zero!");
        while(1);
    }
    timer_period_counts = timer_freq_hz / 1000;
    if (timer_period_counts == 0) {
        timer_period_counts = 1;
    }
    timer_program_local();

    LOG_OK("TIMER: Generic Timer Enabled at 1 kHz.");
}

void timer_init_secondary(void) {
    timer_program_local();
}

void timer_handle_interrupt(void) {
    if (timer_current_core_id() == 0) {
        __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
    }
    if (platform_get()->timer_kind == PLATFORM_TIMER_PHYSICAL) {
        __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(timer_period_counts));
    } else {
        __asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_period_counts));
    }
}
uint64_t timer_get_uptime_seconds(void) {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED) / 1000;
}

uint64_t timer_get_uptime_ms(void) {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

uint64_t timer_get_uptime_ns(void) {
    uint64_t count = 0;
    if (platform_get()->timer_kind == PLATFORM_TIMER_PHYSICAL) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(count));
    } else {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    }
    return ((count - timer_base_count) * 1000000000ULL) / timer_freq_hz;
}
