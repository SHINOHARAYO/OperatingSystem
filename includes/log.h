#pragma once

#include "uart.h"

#define LOG_INFO(msg) do { uart_puts("[INFO] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_OK(msg)   do { uart_puts("[ OK ] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_FAIL(msg) do { uart_puts("[FAIL] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_WARN(msg) do { uart_puts("[WARN] "); uart_puts(msg); uart_puts("\n"); } while(0)

#define LOG_INFO_HEX(msg, val) do { uart_puts("[INFO] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
#define LOG_OK_HEX(msg, val)   do { uart_puts("[ OK ] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
