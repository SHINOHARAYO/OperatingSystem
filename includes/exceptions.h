#pragma once

#include <stdint.h>

void exceptions_init(void);

void handle_sync(uint64_t *regs, uint64_t vector_source);
void handle_irq(void);
void handle_fiq(void);
void handle_serror(void);
