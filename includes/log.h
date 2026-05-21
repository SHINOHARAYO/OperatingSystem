#pragma once

#include "uart.h"

#ifndef LOG_LEVEL
#define LOG_LEVEL 1
#endif

#define LOG_LEVEL_QUIET 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_DEBUG 2
#define LOG_LEVEL_TRACE 3

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(msg) do { uart_puts("[INFO] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_OK(msg)   do { uart_puts("[ OK ] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_WARN(msg) do { uart_puts("[WARN] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_INFO_HEX(msg, val) do { uart_puts("[INFO] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
#define LOG_OK_HEX(msg, val)   do { uart_puts("[ OK ] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
#else
#define LOG_INFO(msg) do { } while(0)
#define LOG_OK(msg) do { } while(0)
#define LOG_WARN(msg) do { } while(0)
#define LOG_INFO_HEX(msg, val) do { (void)(val); } while(0)
#define LOG_OK_HEX(msg, val) do { (void)(val); } while(0)
#endif

#define LOG_FAIL(msg) do { uart_puts("[FAIL] "); uart_puts(msg); uart_puts("\n"); } while(0)

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(msg) do { uart_puts("[DBG ] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_DEBUG_HEX(msg, val) do { uart_puts("[DBG ] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
#else
#define LOG_DEBUG(msg) do { } while(0)
#define LOG_DEBUG_HEX(msg, val) do { (void)(val); } while(0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_TRACE
#define LOG_TRACE(msg) do { uart_puts("[TRCE] "); uart_puts(msg); uart_puts("\n"); } while(0)
#define LOG_TRACE_HEX(msg, val) do { uart_puts("[TRCE] "); uart_puts(msg); uart_hex((uint64_t)(val)); uart_puts("\n"); } while(0)
#else
#define LOG_TRACE(msg) do { } while(0)
#define LOG_TRACE_HEX(msg, val) do { (void)(val); } while(0)
#endif
