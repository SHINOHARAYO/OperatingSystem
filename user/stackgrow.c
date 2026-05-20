#include "lib.h"

static void touch_stack(int depth, volatile uint64_t *sink) {
    volatile uint8_t page[1024];
    page[0] = (uint8_t)depth;
    page[sizeof(page) - 1] = (uint8_t)(depth + 1);
    *sink += page[0] + page[sizeof(page) - 1];

    if (depth > 0) {
        touch_stack(depth - 1, sink);
    }
}

void _start(void) {
    volatile uint64_t sink = 0;
    touch_stack(24, &sink);
    printf("stackgrow: ok %lu\n", sink);
    sys_exit(0);
}
