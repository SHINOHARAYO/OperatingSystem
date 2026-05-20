#pragma once

#include <stdint.h>
#include <stddef.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(uint64_t val);
