#include "lib.h"
#include "ns_proto.h"
#include "fs_proto.h"

#define PAGE_BYTES 4096ULL
#define SPEEDIPC_FILE "speedipc.elf"
#define IPC_STOP 0xFFFFFFFFFFFFFFFFULL
#define BENCH_NS 1000000000ULL

static volatile uint64_t sink;

static uint64_t now_ns(void) {
    return sys_uptime_ns();
}

static uint64_t elapsed_ns_since(uint64_t start) {
    uint64_t current = now_ns();
    return current >= start ? current - start : 0;
}

static void print_rate(const char *name, uint64_t ops, uint64_t ns, const char *unit) {
    if (ns == 0) {
        printf("%s: %lu %s in <1 us\n", name, ops, unit);
        terminal_flush();
        return;
    }

    uint64_t per_sec = (ops * 1000000000ULL) / ns;
    uint64_t ns_per_op = ops ? ns / ops : 0;
    printf("%s: %lu %s in %lu us = %lu %s/s (%lu ns/op)\n",
           name, ops, unit, ns / 1000, per_sec, unit, ns_per_op);
    terminal_flush();
}

static void bench_cpu_loop(void) {
    uint64_t ops = 0;
    uint64_t value = 0x123456789abcdefULL;
    uint64_t start = now_ns();
    uint64_t elapsed = 0;

    while (elapsed < BENCH_NS) {
        for (uint32_t i = 0; i < 200000; i++) {
            value = (value * 6364136223846793005ULL) + 1;
            value ^= value >> 33;
        }
        ops += 200000;
        elapsed = elapsed_ns_since(start);
    }

    sink = value;
    print_rate("cpu-loop", ops, elapsed, "ops");
}

static void bench_syscall(void) {
    uint64_t ops = 0;
    uint64_t start = now_ns();
    uint64_t elapsed = 0;

    while (elapsed < BENCH_NS) {
        for (uint32_t i = 0; i < 2000; i++) {
            sink ^= sys_uptime_ms();
        }
        ops += 2000;
        elapsed = elapsed_ns_since(start);
    }

    print_rate("sys-uptime-ms", ops, elapsed, "calls");
}

static void bench_yield(void) {
    uint64_t ops = 0;
    uint64_t start = now_ns();
    uint64_t elapsed = 0;

    while (elapsed < BENCH_NS) {
        for (uint32_t i = 0; i < 16; i++) {
            sys_yield();
        }
        ops += 16;
        elapsed = elapsed_ns_since(start);
    }

    print_rate("yield", ops, elapsed, "calls");
}

static void bench_memory(void) {
    uint64_t pages = 2048;
    uint64_t size = pages * PAGE_BYTES;
    volatile uint8_t *buf = (volatile uint8_t *)sys_mmap(size);
    if (!buf) {
        printf("lazy-fault: mmap failed\n");
        return;
    }

    uint64_t start = now_ns();
    for (uint64_t i = 0; i < pages; i++) {
        buf[i * PAGE_BYTES] = (uint8_t)i;
    }
    uint64_t elapsed = elapsed_ns_since(start);
    print_rate("lazy-fault", pages, elapsed, "pages");

    volatile uint64_t *words = (volatile uint64_t *)buf;
    uint64_t word_count = size / sizeof(uint64_t);
    uint64_t bytes = 0;
    uint64_t pattern = 0xA5A5A5A500000000ULL;
    start = now_ns();
    elapsed = 0;

    while (elapsed < BENCH_NS) {
        for (uint64_t i = 0; i < word_count; i++) {
            words[i] = pattern | i;
        }
        bytes += size;
        pattern++;
        elapsed = elapsed_ns_since(start);
    }

    print_rate("mem-write", bytes, elapsed, "bytes");
}


static void bench_ipc(void) {
#if NEPTUNE_PLATFORM_PI4
    spawn_result_t helper = sys_spawn_file2(SPEEDIPC_FILE, 5);
#else
    spawn_result_t helper = vfs_spawn_program(SPEEDIPC_FILE, 5);
#endif

    if ((int)helper.tid < 0 || helper.endpoint_cap < 0) {
        printf("ipc-call: helper spawn failed\n");
        return;
    }

    int cap = helper.endpoint_cap;

    uint64_t ops = 0;
    uint64_t start = now_ns();
    uint64_t elapsed = 0;
    int ok = 1;

    while (elapsed < BENCH_NS && ok) {
        for (uint32_t i = 0; i < 64; i++) {
            uint64_t payload[IPC_INLINE_WORDS] = {ops};
            ipc_msg_t reply = sys_ipc_call((uint32_t)cap, 0, 8, payload);
            if (reply.status != 0 || reply.payload[0] != ops + 1) {
                ok = 0;
                break;
            }
            ops++;
        }
        elapsed = elapsed_ns_since(start);
    }

    uint64_t stop[IPC_INLINE_WORDS] = {IPC_STOP};
    (void)sys_ipc_call((uint32_t)cap, 0, 8, stop);
    wait_info_t status = sys_wait(helper.tid);

    if (!ok || status.reason != TASK_TERM_EXITED || status.exit_code != 0) {
        printf("ipc-call: failed ops=%lu child_reason=%u code=%d\n",
               ops, status.reason, status.exit_code);
        return;
    }

    print_rate("ipc-call", ops, elapsed, "roundtrips");
}

void _start(void) {
    printf("speed: starting benchmark\n");
    printf("timer: generic counter nanoseconds\n");
    terminal_flush();

    bench_cpu_loop();
    bench_syscall();
    bench_yield();
    bench_memory();
    bench_ipc();

    printf("speed: done\n");
    terminal_flush();
    sys_exit(0);
}
