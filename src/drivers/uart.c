#include "uart.h"
#include "platform.h"

static volatile uint32_t *uart_reg(uint64_t offset) {
    return (volatile uint32_t *)(platform_get()->uart_pa + offset);
}

void uart_init(void) {
    *uart_reg(0x030) = 0;
    if (platform_is_pi4()) {
        *uart_reg(0x024) = 26;
        *uart_reg(0x028) = 3;
    } else {
        *uart_reg(0x024) = 13;
        *uart_reg(0x028) = 1;
    }
    *uart_reg(0x02C) = (3 << 5) | (1 << 4);
    *uart_reg(0x030) = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(char c) {
    while (*uart_reg(0x018) & (1 << 5)) {
    }
    *uart_reg(0x000) = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

void uart_hex(uint64_t val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        if (nibble < 10) {
            uart_putc('0' + nibble);
        } else {
            uart_putc('A' + (nibble - 10));
        }
    }
}
