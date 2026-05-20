#include "uart.h"

// QEMU virt machine PL011 UART base address
#define UART_BASE 0x09000000

#define UART_DR   (*(volatile uint32_t *)(UART_BASE + 0x000))
#define UART_FR   (*(volatile uint32_t *)(UART_BASE + 0x018))
#define UART_IBRD (*(volatile uint32_t *)(UART_BASE + 0x024))
#define UART_FBRD (*(volatile uint32_t *)(UART_BASE + 0x028))
#define UART_LCRH (*(volatile uint32_t *)(UART_BASE + 0x02C))
#define UART_CR   (*(volatile uint32_t *)(UART_BASE + 0x030))

void uart_init(void) {
    UART_CR = 0;

    // Set baud rate (assuming 24MHz UART clock)
    // 115200 baud -> IBRD=13, FBRD=1
    UART_IBRD = 13;
    UART_FBRD = 1;

    // 8 bit data, 1 stop bit, no parity, FIFOs enabled
    UART_LCRH = (3 << 5) | (1 << 4);
    UART_CR = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(char c) {
    while (UART_FR & (1 << 5)) {
    }
    UART_DR = c;
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
