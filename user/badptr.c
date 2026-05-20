#include "lib.h"

void _start(void) {
    printf("badptr: passing invalid pointers to syscalls\n");

    uint32_t tid = sys_spawn_file((const char *)0xDEADBEEFULL, 50);
    printf("badptr: sys_spawn_file(invalid) -> %d\n", (int)tid);

    tid = sys_spawn((const uint8_t *)0xDEADBEEFULL, 64, 50);
    printf("badptr: sys_spawn(invalid) -> %d\n", (int)tid);

    sys_exit(0);
}
