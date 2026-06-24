#pragma once

#include <stdint.h>

typedef struct {
    uint64_t calls;
    uint64_t direct_calls;
    uint64_t slow_calls;
    uint64_t replies;
    uint64_t cap_lookup_cycles;
    uint64_t direct_roundtrip_cycles;
    uint64_t total_roundtrip_cycles;
    uint64_t counter_hz;
} ipc_profile_t;

int ipc_call_syscall(uint64_t *regs);
void ipc_recv_syscall(uint64_t *regs);
void ipc_recv_timeout_syscall(uint64_t *regs, uint64_t timeout_ms);
int ipc_reply_syscall(uint64_t *regs);
void ipc_reply_recv_syscall(uint64_t *regs);
void ipc_profile_syscall(uint64_t *regs);
void ipc_task_died(uint32_t tid);
