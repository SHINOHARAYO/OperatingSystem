#include "lib.h"

#define BELOW_STACK_GROWTH_LIMIT 0x8FFF0000ULL

void _start(void) {
    volatile uint64_t *bad = (volatile uint64_t *)BELOW_STACK_GROWTH_LIMIT;
    *bad = 0xBADF00DULL;
    sys_exit(1);
}
