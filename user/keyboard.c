#include "lib.h"
#include "ns_proto.h"

// PL011 UART MMIO page mapped by the kernel for this driver task.
#define UART_BASE 0xB0000000

#define UART_DR ((volatile uint32_t *)(UART_BASE + 0x000))
#define UART_FR ((volatile uint32_t *)(UART_BASE + 0x018))
#define UART_IMSC ((volatile uint32_t *)(UART_BASE + 0x038))
#define UART_ICR ((volatile uint32_t *)(UART_BASE + 0x044))

#define UART_INT_RX (1 << 4)
#define UART_INT_RXTIME (1 << 6)

#define KEYBOARD_IDLE_SLEEP_MS 10

static int shell_cap = -1;

static int get_shell_cap(void) {
  while (shell_cap < 0) {
    shell_cap = ns_resolve("shell");
    if (shell_cap < 0) {
      sys_sleep(10);
    }
  }
  return shell_cap;
}

static int forward_pending_input(void) {
  int delivered = 0;

  // UART_FR bit 4 is RXFE (receive FIFO empty).
  while ((*UART_FR & (1 << 4)) == 0) {
    char c = (char)(*UART_DR & 0xFF);
    uint64_t payload[IPC_INLINE_WORDS] = {0};
    payload[0] = (uint64_t)c;
    sys_ipc_call((uint32_t)get_shell_cap(), 0, 8, payload);
    delivered = 1;
  }

  return delivered;
}

void _start(void) {
  while (ns_register("keyboard") < 0) {
    sys_sleep(10);
  }

  // Clear stale RX interrupt state, then unmask RX interrupts.
  *UART_ICR = UART_INT_RX | UART_INT_RXTIME;

  // RXIM fires at the FIFO trigger level; RTIM covers short interactive input.
  *UART_IMSC |= UART_INT_RX | UART_INT_RXTIME;

  while (1) {
    int delivered = forward_pending_input();

    *UART_ICR = UART_INT_RX | UART_INT_RXTIME;

    if (!delivered) {
      sys_sleep(KEYBOARD_IDLE_SLEEP_MS);
    }
  }
}
