#pragma once

#include <stdint.h>

int ipc_call_syscall(uint64_t *regs);
void ipc_recv_syscall(uint64_t *regs);
int ipc_reply_syscall(uint64_t *regs);
