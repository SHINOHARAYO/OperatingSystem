#include "lib.h"
#include "ns_proto.h"

#define SERVICE_PRIORITY 5
#define SHELL_PRIORITY 10

static void spawn_boot_task(const char *name, uint8_t priority) {
    for (uint32_t attempt = 0; attempt < 40; attempt++) {
        spawn_result_t result = sys_spawn_file2(name, priority);
        if ((int)result.tid >= 0) {
            return;
        }
        sys_sleep(10);
    }
}

static void wait_for_service(const char *name) {
    for (uint32_t attempt = 0; attempt < 200; attempt++) {
        if (ns_resolve(name) >= 0) {
            return;
        }
        sys_sleep(10);
    }
}

void _start(void) {
    spawn_boot_task("ns.elf", SERVICE_PRIORITY);
    wait_for_service("ns");

    spawn_boot_task("uart.elf", SERVICE_PRIORITY);
    spawn_boot_task("keyboard.elf", SERVICE_PRIORITY);
    wait_for_service("uart");
    wait_for_service("keyboard");

    spawn_boot_task("shell.elf", SHELL_PRIORITY);
    while (1) {
        sys_sleep(1000);
    }
}
