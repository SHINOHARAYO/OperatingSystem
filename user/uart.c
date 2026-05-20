#include "lib.h"
#include "ns_proto.h"

// The kernel maps the PL011 UART MMIO page here for this driver task.
#define UART_BASE 0xB0000000

#define UART_DR   ((volatile uint32_t *)(UART_BASE + 0x00))
#define UART_FR   ((volatile uint32_t *)(UART_BASE + 0x18))
#define UART_FR_TXFF (1 << 5)

static void uart_putc(char c) {
    while (*UART_FR & UART_FR_TXFF) {
    }
    *UART_DR = c;
}

static void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

void _start(void) {
    while (ns_register("uart") < 0) {
        sys_sleep(10);
    }

    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        
        char *str = (char *)&msg.payload;
        
        for (int i = 0; i < (int)msg.len && i < (int)IPC_INLINE_BYTES; i++) {
            if (str[i] == '\0') {
                break;
            }
            uart_putc(str[i]);
        }
        
        sys_ipc_reply(msg.reply_cap, 0, 0, 0, 0);
    }
}
