#pragma once

#include <stdint.h>
#include "sched.h"

#define PIPE_WOULD_BLOCK (-2L)

void pipe_init(void);
void pipe_create_syscall(uint64_t *regs);
void pipe_read_syscall(uint64_t *regs, uint32_t cap, uint64_t buffer,
                       uint64_t count);
void pipe_write_syscall(uint64_t *regs, uint32_t cap, uint64_t buffer,
                        uint64_t count);
void pipe_close_syscall(uint64_t *regs, uint32_t cap);
void pipe_dup_syscall(uint64_t *regs, uint32_t cap);
int pipe_inherit_cap(tcb_t *parent, tcb_t *child, uint32_t parent_cap,
                     uint32_t child_slot);
