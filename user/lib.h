#ifndef _USER_LIB_H_
#define _USER_LIB_H_

typedef unsigned long  uint64_t;
typedef   signed long  int64_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;

typedef struct {
    uint32_t present;
    uint32_t tid;
    uint32_t state;
    uint8_t priority;
    uint8_t base_priority;
    uint32_t ticks_remaining;
    uint32_t wait_time;
    uint32_t ipc_target_tid;
    uint32_t awaiting_irq;
    uint32_t parent_tid;
} task_info_t;

typedef struct {
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t heap_size;
    uint64_t heap_used;
    uint64_t heap_mapped;
} memory_info_t;

typedef struct {
    uint32_t present;
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    uint64_t count;
    uint64_t committed_pages;
    uint64_t max_pages;
    uint32_t backing_type;
} vma_info_t;

typedef struct {
    uint32_t present;
    uint32_t slot;
    uint32_t type;
    uint32_t object_id;
    uint64_t rights;
    uint64_t flags;
    uint32_t capacity;
} cap_info_t;

typedef struct {
    uint32_t log_level;
    uint32_t tick_hz;
    uint32_t task_capacity;
    uint32_t max_user_asids;
    uint64_t user_stack_max_size;
    uint32_t max_mem_objects;
    uint32_t max_mem_mappings;
    uint32_t active_mem_objects;
    uint32_t active_mem_mappings;
    uint32_t current_vma_count;
    uint32_t current_vma_capacity;
} debug_info_t;

#define VMA_READ       (1ULL << 0)
#define VMA_WRITE      (1ULL << 1)
#define VMA_EXEC       (1ULL << 2)
#define VMA_USER       (1ULL << 3)
#define VMA_GROWS_DOWN (1ULL << 4)
#define VMA_LAZY       (1ULL << 5)
#define VMA_FILE       (1ULL << 6)
#define VMA_GUARD      (1ULL << 7)
#define VMA_STACK      (1ULL << 8)
#define VMA_MMAP       (1ULL << 9)
#define VMA_ELF        (1ULL << 10)

#define VMA_BACKING_NONE   0
#define VMA_BACKING_ANON   1
#define VMA_BACKING_FILE   2
#define VMA_BACKING_DEVICE 3

#define MEM_RIGHT_READ     (1ULL << 0)
#define MEM_RIGHT_WRITE    (1ULL << 1)
#define MEM_RIGHT_SHARE    (1ULL << 2)
#define MEM_RIGHT_TRANSFER (1ULL << 3)
#define MEM_RIGHT_LEND     (1ULL << 4)

#define OCAP_NONE     0
#define OCAP_ENDPOINT 1
#define OCAP_EXEC     2
#define OCAP_FRAME    3
#define OCAP_VMA      4
#define OCAP_REPLY    5
#define OCAP_FILE     6

#define OCAP_RIGHT_READ     (1ULL << 0)
#define OCAP_RIGHT_WRITE    (1ULL << 1)
#define OCAP_RIGHT_CALL     (1ULL << 2)
#define OCAP_RIGHT_REPLY    (1ULL << 3)
#define OCAP_RIGHT_SPAWN    (1ULL << 4)
#define OCAP_RIGHT_MAP      (1ULL << 5)
#define OCAP_RIGHT_SHARE    (1ULL << 6)
#define OCAP_RIGHT_TRANSFER (1ULL << 7)
#define OCAP_RIGHT_LEND     (1ULL << 8)
#define OCAP_RIGHT_REVOKE   (1ULL << 9)

#define CAP_SELF 1
#define CAP_NS 2
#define IPC_INLINE_WORDS 16
#define IPC_INLINE_BYTES 128
#define IPC_REPLY_INLINE_WORDS 15
#define IPC_FLAG_MEM (1ULL << 0)
#define IPC_FLAG_CAP (1ULL << 1)
#define IPC_MEM_MODE_MASK     (0xFULL << 8)
#define IPC_MEM_MODE_SHARE    (1ULL << 8)
#define IPC_MEM_MODE_TRANSFER (2ULL << 8)
#define IPC_MEM_MODE_LEND     (3ULL << 8)

typedef struct {
    int status;
    uint32_t tid;
    uint32_t reason;
    int exit_code;
    uint64_t esr;
    uint64_t elr;
    uint64_t far;
} wait_info_t;

#define TASK_TERM_EXITED 1
#define TASK_TERM_KILLED 2
#define TASK_TERM_FAULTED 3

#define WAIT_ERR_NO_CHILD 1
#define WAIT_ERR_SELF 2
#define WAIT_ERR_INVALID 3

#define WAIT_STATUS_DONE 0
#define WAIT_STATUS_RUNNING 1

void printf(const char *fmt, ...);
void puts(const char *s);

static inline void sys_yield(void) {
    asm volatile("mov x8, #1\n\tsvc #0" : : : "memory");
}

static inline void sys_exit(int code) {
    register uint64_t x0 asm("x0") = (uint64_t)code;
    register uint64_t x8 asm("x8") = 8;
    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    while(1);
}

typedef struct {
    int status;
    uint32_t reply_cap;
    uint32_t sender_tid;
    uint64_t flags;
    uint64_t len;
    uint64_t payload[IPC_INLINE_WORDS];
} ipc_msg_t;

typedef struct {
    uint32_t tid;
    int endpoint_cap;
} spawn_result_t;

typedef struct {
    int status;
    uint64_t value0;
    uint64_t value1;
    uint64_t value2;
} vfs_reply_t;

typedef struct {
    int status;
    uint32_t vfs_id;
    uint32_t client_tid;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
} vfs_request_t;

static inline ipc_msg_t sys_ipc_call(uint32_t cap, uint64_t flags, uint64_t len, const uint64_t payload[IPC_INLINE_WORDS]) {
    register uint64_t x0 asm("x0") = cap;
    register uint64_t x1 asm("x1") = flags;
    register uint64_t x2 asm("x2") = len;
    register uint64_t x3 asm("x3") = payload ? payload[0] : 0;
    register uint64_t x4 asm("x4") = payload ? payload[1] : 0;
    register uint64_t x5 asm("x5") = payload ? payload[2] : 0;
    register uint64_t x6 asm("x6") = payload ? payload[3] : 0;
    register uint64_t x7 asm("x7") = payload ? payload[4] : 0;
    register uint64_t x8 asm("x8") = payload ? payload[5] : 0;
    register uint64_t x9 asm("x9") = payload ? payload[6] : 0;
    register uint64_t x10 asm("x10") = payload ? payload[7] : 0;
    register uint64_t x11 asm("x11") = payload ? payload[8] : 0;
    register uint64_t x12 asm("x12") = payload ? payload[9] : 0;
    register uint64_t x13 asm("x13") = payload ? payload[10] : 0;
    register uint64_t x14 asm("x14") = payload ? payload[11] : 0;
    register uint64_t x15 asm("x15") = payload ? payload[12] : 0;
    register uint64_t x16 asm("x16") = payload ? payload[13] : 0;
    register uint64_t x17 asm("x17") = payload ? payload[14] : 0;
    register uint64_t x18 asm("x18") = payload ? payload[15] : 0;

    asm volatile(
        "svc #35"
        : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
          "+r" (x5), "+r" (x6), "+r" (x7), "+r" (x8), "+r" (x9), "+r" (x10),
          "+r" (x11), "+r" (x12), "+r" (x13), "+r" (x14), "+r" (x15),
          "+r" (x16), "+r" (x17), "+r" (x18)
        :
        : "memory"
    );

    ipc_msg_t msg;
    msg.status = (int)x0;
    msg.reply_cap = 0;
    msg.sender_tid = 0;
    msg.flags = x1;
    msg.len = x2;
    msg.payload[0] = x3;
    msg.payload[1] = x4;
    msg.payload[2] = x5;
    msg.payload[3] = x6;
    msg.payload[4] = x7;
    msg.payload[5] = x8;
    msg.payload[6] = x9;
    msg.payload[7] = x10;
    msg.payload[8] = x11;
    msg.payload[9] = x12;
    msg.payload[10] = x13;
    msg.payload[11] = x14;
    msg.payload[12] = x15;
    msg.payload[13] = x16;
    msg.payload[14] = x17;
    msg.payload[15] = x18;
    return msg;
}

static inline ipc_msg_t sys_ipc_recv(uint32_t cap) {
    register uint64_t x0 asm("x0") = cap;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x7 asm("x7");
    register uint64_t x8 asm("x8");
    register uint64_t x9 asm("x9");
    register uint64_t x10 asm("x10");
    register uint64_t x11 asm("x11");
    register uint64_t x12 asm("x12");
    register uint64_t x13 asm("x13");
    register uint64_t x14 asm("x14");
    register uint64_t x15 asm("x15");
    register uint64_t x16 asm("x16");
    register uint64_t x17 asm("x17");
    register uint64_t x18 asm("x18");

    asm volatile(
        "svc #36"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4),
          "=r" (x5), "=r" (x6), "=r" (x7), "=r" (x8), "=r" (x9), "=r" (x10),
          "=r" (x11), "=r" (x12), "=r" (x13), "=r" (x14), "=r" (x15),
          "=r" (x16), "=r" (x17), "=r" (x18)
        :
        : "memory"
    );

    ipc_msg_t msg;
    msg.status = (int)x0;
    msg.reply_cap = (uint32_t)x0;
    msg.sender_tid = (uint32_t)x1;
    msg.flags = 0;
    msg.len = x2;
    msg.payload[0] = x3;
    msg.payload[1] = x4;
    msg.payload[2] = x5;
    msg.payload[3] = x6;
    msg.payload[4] = x7;
    msg.payload[5] = x8;
    msg.payload[6] = x9;
    msg.payload[7] = x10;
    msg.payload[8] = x11;
    msg.payload[9] = x12;
    msg.payload[10] = x13;
    msg.payload[11] = x14;
    msg.payload[12] = x15;
    msg.payload[13] = x16;
    msg.payload[14] = x17;
    msg.payload[15] = x18;
    return msg;
}

static inline int sys_ipc_reply(uint32_t reply_cap, int status, uint64_t flags, uint64_t len, const uint64_t payload[IPC_REPLY_INLINE_WORDS]) {
    register uint64_t x0 asm("x0") = reply_cap;
    register uint64_t x1 asm("x1") = (uint64_t)status;
    register uint64_t x2 asm("x2") = flags;
    register uint64_t x3 asm("x3") = len;
    register uint64_t x4 asm("x4") = payload ? payload[0] : 0;
    register uint64_t x5 asm("x5") = payload ? payload[1] : 0;
    register uint64_t x6 asm("x6") = payload ? payload[2] : 0;
    register uint64_t x7 asm("x7") = payload ? payload[3] : 0;
    register uint64_t x8 asm("x8") = payload ? payload[4] : 0;
    register uint64_t x9 asm("x9") = payload ? payload[5] : 0;
    register uint64_t x10 asm("x10") = payload ? payload[6] : 0;
    register uint64_t x11 asm("x11") = payload ? payload[7] : 0;
    register uint64_t x12 asm("x12") = payload ? payload[8] : 0;
    register uint64_t x13 asm("x13") = payload ? payload[9] : 0;
    register uint64_t x14 asm("x14") = payload ? payload[10] : 0;
    register uint64_t x15 asm("x15") = payload ? payload[11] : 0;
    register uint64_t x16 asm("x16") = payload ? payload[12] : 0;
    register uint64_t x17 asm("x17") = payload ? payload[13] : 0;
    register uint64_t x18 asm("x18") = payload ? payload[14] : 0;

    asm volatile(
        "svc #37"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5),
          "r" (x6), "r" (x7), "r" (x8), "r" (x9), "r" (x10),
          "r" (x11), "r" (x12), "r" (x13), "r" (x14), "r" (x15),
          "r" (x16), "r" (x17), "r" (x18)
        : "memory"
    );

    return (int)x0;
}

static inline uint32_t sys_spawn_exec(uint32_t exec_cap, uint8_t priority) {
    register uint64_t x0 asm("x0") = exec_cap;
    register uint64_t x1 asm("x1") = priority;
    register uint64_t x8 asm("x8") = 24;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x8)
        : "memory"
    );

    return (uint32_t)x0;
}

static inline spawn_result_t sys_spawn_exec2(uint32_t exec_cap, uint8_t priority) {
    register uint64_t x0 asm("x0") = exec_cap;
    register uint64_t x1 asm("x1") = priority;
    register uint64_t x8 asm("x8") = 24;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1)
        : "r" (x8)
        : "memory"
    );

    spawn_result_t result;
    result.tid = (uint32_t)x0;
    result.endpoint_cap = (int)x1;
    return result;
}

static inline void* sys_mmap(uint64_t size) {
    register uint64_t x0 asm("x0") = size;
    register uint64_t x8 asm("x8") = 4;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (void*)x0;
}

static inline void sys_sleep(uint64_t ms) {
    register uint64_t x0 asm("x0") = ms;
    register uint64_t x8 asm("x8") = 5;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );
}

static inline uint32_t sys_spawn(const uint8_t *elf_data, uint64_t elf_size, uint8_t priority) {
    register uint64_t x0 asm("x0") = (uint64_t)elf_data;
    register uint64_t x1 asm("x1") = elf_size;
    register uint64_t x2 asm("x2") = (uint64_t)priority;
    register uint64_t x8 asm("x8") = 6;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (uint32_t)x0;
}

static inline spawn_result_t sys_spawn2(const uint8_t *elf_data, uint64_t elf_size, uint8_t priority) {
    register uint64_t x0 asm("x0") = (uint64_t)elf_data;
    register uint64_t x1 asm("x1") = elf_size;
    register uint64_t x2 asm("x2") = (uint64_t)priority;
    register uint64_t x8 asm("x8") = 6;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1)
        : "r" (x2), "r" (x8)
        : "memory"
    );

    spawn_result_t result;
    result.tid = (uint32_t)x0;
    result.endpoint_cap = (int)x1;
    return result;
}

static inline uint32_t sys_spawn_file(const char *name, uint8_t priority) {
    register uint64_t x0 asm("x0") = (uint64_t)name;
    register uint64_t x1 asm("x1") = (uint64_t)priority;
    register uint64_t x8 asm("x8") = 16;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x8)
        : "memory"
    );

    return (uint32_t)x0;
}

static inline int64_t sys_file_stat(uint32_t file_cap, char *name, uint64_t name_cap) {
    register uint64_t x0 asm("x0") = (uint64_t)file_cap;
    register uint64_t x1 asm("x1") = (uint64_t)name;
    register uint64_t x2 asm("x2") = name_cap;
    register uint64_t x8 asm("x8") = 18;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (int64_t)x0;
}

static inline void sys_await_irq(uint32_t irq_num) {
    register uint64_t x0 asm("x0") = (uint64_t)irq_num;
    register uint64_t x8 asm("x8") = 7;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );
}

static inline int sys_await_irq_timeout(uint32_t irq_num, uint64_t timeout_ms) {
    register uint64_t x0 asm("x0") = (uint64_t)irq_num;
    register uint64_t x1 asm("x1") = timeout_ms;
    register uint64_t x8 asm("x8") = 12;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1)
        : "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline uint64_t sys_uptime(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x8 asm("x8") = 9;

    asm volatile(
        "svc #0"
        : "=r" (x0)
        : "r" (x8)
        : "memory"
    );

    return x0;
}

static inline uint64_t sys_uptime_ms(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x8 asm("x8") = 39;

    asm volatile(
        "svc #0"
        : "=r" (x0)
        : "r" (x8)
        : "memory"
    );

    return x0;
}

static inline uint64_t sys_uptime_ns(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x8 asm("x8") = 40;

    asm volatile(
        "svc #0"
        : "=r" (x0)
        : "r" (x8)
        : "memory"
    );

    return x0;
}

static inline task_info_t sys_ps(uint32_t index) {
    register uint64_t x0 asm("x0") = (uint64_t)index;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x8 asm("x8") = 10;

    asm volatile(
        "svc #0"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4), "=r" (x5), "=r" (x6)
        : "r" (x8)
        : "memory"
    );

    task_info_t info;
    info.present = (uint32_t)x0;
    info.tid = (uint32_t)x1;
    info.state = (uint32_t)x2;
    info.priority = (uint8_t)(x3 >> 8);
    info.base_priority = (uint8_t)x3;
    info.ticks_remaining = (uint32_t)(x4 >> 32);
    info.wait_time = (uint32_t)x4;
    info.ipc_target_tid = (uint32_t)x5;
    info.awaiting_irq = (uint32_t)x6;
    info.parent_tid = (uint32_t)(x6 >> 32);
    return info;
}

static inline int sys_kill(uint32_t tid) {
    register uint64_t x0 asm("x0") = (uint64_t)tid;
    register uint64_t x8 asm("x8") = 11;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline memory_info_t sys_mem(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x8 asm("x8") = 13;

    asm volatile(
        "svc #0"
        : "=r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4)
        : "r" (x8)
        : "memory"
    );

    memory_info_t info;
    info.total_memory = x0;
    info.free_memory = x1;
    info.heap_size = x2;
    info.heap_used = x3;
    info.heap_mapped = x4;
    return info;
}

static inline spawn_result_t sys_spawn_file2(const char *name, uint8_t priority) {
    register uint64_t x0 asm("x0") = (uint64_t)name;
    register uint64_t x1 asm("x1") = (uint64_t)priority;
    register uint64_t x8 asm("x8") = 16;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1)
        : "r" (x8)
        : "memory"
    );

    spawn_result_t result;
    result.tid = (uint32_t)x0;
    result.endpoint_cap = (int)x1;
    return result;
}

static inline vma_info_t sys_vmmap(uint32_t index) {
    register uint64_t x0 asm("x0") = (uint64_t)index;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x7 asm("x7");
    register uint64_t x8 asm("x8") = 25;

    asm volatile(
        "svc #0"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4), "=r" (x5), "=r" (x6), "=r" (x7)
        : "r" (x8)
        : "memory"
    );

    vma_info_t info;
    info.present = (uint32_t)x0;
    info.start = x1;
    info.end = x2;
    info.flags = x3;
    info.count = x4;
    info.committed_pages = x5;
    info.max_pages = x6;
    info.backing_type = (uint32_t)x7;
    return info;
}

static inline int sys_mem_export(void *addr, uint64_t size, uint64_t rights) {
    register uint64_t x0 asm("x0") = (uint64_t)addr;
    register uint64_t x1 asm("x1") = size;
    register uint64_t x2 asm("x2") = rights;
    register uint64_t x8 asm("x8") = 26;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline void *sys_mem_share(uint32_t target_tid, uint32_t mem_cap, uint64_t dst_hint) {
    register uint64_t x0 asm("x0") = target_tid;
    register uint64_t x1 asm("x1") = mem_cap;
    register uint64_t x2 asm("x2") = dst_hint;
    register uint64_t x8 asm("x8") = 27;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (void *)x0;
}

static inline void *sys_mem_transfer(uint32_t target_tid, uint32_t mem_cap, uint64_t dst_hint) {
    register uint64_t x0 asm("x0") = target_tid;
    register uint64_t x1 asm("x1") = mem_cap;
    register uint64_t x2 asm("x2") = dst_hint;
    register uint64_t x8 asm("x8") = 28;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (void *)x0;
}

static inline int sys_munmap(void *addr, uint64_t size) {
    register uint64_t x0 asm("x0") = (uint64_t)addr;
    register uint64_t x1 asm("x1") = size;
    register uint64_t x8 asm("x8") = 29;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline void *sys_mem_lend(uint32_t target_tid, uint32_t mem_cap, uint64_t dst_hint) {
    register uint64_t x0 asm("x0") = target_tid;
    register uint64_t x1 asm("x1") = mem_cap;
    register uint64_t x2 asm("x2") = dst_hint;
    register uint64_t x8 asm("x8") = 30;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x8)
        : "memory"
    );

    return (void *)x0;
}

static inline int sys_mem_revoke(uint32_t mem_cap) {
    register uint64_t x0 asm("x0") = mem_cap;
    register uint64_t x8 asm("x8") = 31;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline int sys_fork(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x8 asm("x8") = 32;

    asm volatile(
        "svc #0"
        : "=r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline void *sys_file_mmap(uint32_t file_cap) {
    register uint64_t x0 asm("x0") = file_cap;
    register uint64_t x8 asm("x8") = 33;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (void *)x0;
}

static inline uint32_t sys_task_capacity(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x8 asm("x8") = 34;

    asm volatile(
        "svc #0"
        : "=r" (x0)
        : "r" (x8)
        : "memory"
    );

    return (uint32_t)x0;
}

static inline cap_info_t sys_capstat(uint32_t slot) {
    register uint64_t x0 asm("x0") = (uint64_t)slot;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x8 asm("x8") = 38;

    asm volatile(
        "svc #0"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3),
          "=r" (x4), "=r" (x5), "=r" (x6)
        : "r" (x8)
        : "memory"
    );

    cap_info_t info;
    info.present = (uint32_t)x0;
    info.slot = (uint32_t)x1;
    info.type = (uint32_t)x2;
    info.object_id = (uint32_t)x3;
    info.rights = x4;
    info.flags = x5;
    info.capacity = (uint32_t)x6;
    return info;
}

static inline debug_info_t sys_debug_info(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x7 asm("x7");
    register uint64_t x8 asm("x8") = 41;

    asm volatile(
        "svc #0"
        : "=r" (x0), "=r" (x1), "=r" (x2), "=r" (x3),
          "=r" (x4), "=r" (x5), "=r" (x6), "=r" (x7)
        : "r" (x8)
        : "memory"
    );

    debug_info_t info;
    info.log_level = (uint32_t)x0;
    info.tick_hz = (uint32_t)x1;
    info.task_capacity = (uint32_t)x2;
    info.max_user_asids = (uint32_t)x3;
    info.user_stack_max_size = x4;
    info.max_mem_objects = (uint32_t)(x5 >> 32);
    info.max_mem_mappings = (uint32_t)x5;
    info.active_mem_objects = (uint32_t)(x6 >> 32);
    info.active_mem_mappings = (uint32_t)x6;
    info.current_vma_count = (uint32_t)(x7 >> 32);
    info.current_vma_capacity = (uint32_t)x7;
    return info;
}

static inline int sys_vfs_bind(uint32_t vfs_id, uint32_t endpoint_cap) {
    register uint64_t x0 asm("x0") = vfs_id;
    register uint64_t x1 asm("x1") = endpoint_cap;
    register uint64_t x8 asm("x8") = 42;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline vfs_reply_t sys_vfs_call(uint32_t vfs_id,
                                       uint64_t arg0,
                                       uint64_t arg1,
                                       uint64_t arg2) {
    register uint64_t x0 asm("x0") = vfs_id;
    register uint64_t x1 asm("x1") = arg0;
    register uint64_t x2 asm("x2") = arg1;
    register uint64_t x3 asm("x3") = arg2;
    register uint64_t x8 asm("x8") = 43;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
        : "r" (x8)
        : "memory"
    );

    vfs_reply_t reply;
    reply.status = (int)x0;
    reply.value0 = x1;
    reply.value1 = x2;
    reply.value2 = x3;
    return reply;
}

static inline int sys_vfs_reply(int status,
                                uint64_t value0,
                                uint64_t value1,
                                uint64_t value2) {
    register uint64_t x0 asm("x0") = (uint64_t)status;
    register uint64_t x1 asm("x1") = value0;
    register uint64_t x2 asm("x2") = value1;
    register uint64_t x3 asm("x3") = value2;
    register uint64_t x8 asm("x8") = 44;

    asm volatile(
        "svc #0"
        : "+r" (x0)
        : "r" (x1), "r" (x2), "r" (x3), "r" (x8)
        : "memory"
    );

    return (int)x0;
}

static inline void *sys_vfs_inject(uint32_t client_tid,
                                   void *client_va,
                                   uint64_t page_count) {
    register uint64_t x0 asm("x0") = client_tid;
    register uint64_t x1 asm("x1") = (uint64_t)client_va;
    register uint64_t x2 asm("x2") = page_count;
    register uint64_t x8 asm("x8") = 45;

    asm volatile(
        "svc #0"
        : "+r" (x0), "+r" (x1), "+r" (x2)
        : "r" (x8)
        : "memory"
    );

    return (void *)x0;
}

static inline vfs_request_t sys_vfs_recv(void) {
    register uint64_t x0 asm("x0");
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x8 asm("x8") = 46;

    asm volatile(
        "svc #0"
        : "=r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4)
        : "r" (x8)
        : "memory"
    );

    vfs_request_t request;
    request.status = (int)x0 < 0 ? -1 : 0;
    request.vfs_id = (uint32_t)x0;
    request.arg0 = x1;
    request.arg1 = x2;
    request.arg2 = x3;
    request.client_tid = (uint32_t)x4;
    return request;
}

static inline wait_info_t sys_wait(uint32_t tid) {
    register uint64_t x0 asm("x0") = (uint64_t)tid;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x8 asm("x8") = 14;

    asm volatile(
        "svc #0"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4), "=r" (x5), "=r" (x6)
        : "r" (x8)
        : "memory"
    );

    wait_info_t info;
    info.status = (int)x0;
    info.tid = (uint32_t)x1;
    info.reason = (uint32_t)x2;
    info.exit_code = (int)x3;
    info.esr = x4;
    info.elr = x5;
    info.far = x6;
    return info;
}

static inline wait_info_t sys_poll(uint32_t tid) {
    register uint64_t x0 asm("x0") = (uint64_t)tid;
    register uint64_t x1 asm("x1");
    register uint64_t x2 asm("x2");
    register uint64_t x3 asm("x3");
    register uint64_t x4 asm("x4");
    register uint64_t x5 asm("x5");
    register uint64_t x6 asm("x6");
    register uint64_t x8 asm("x8") = 15;

    asm volatile(
        "svc #0"
        : "+r" (x0), "=r" (x1), "=r" (x2), "=r" (x3), "=r" (x4), "=r" (x5), "=r" (x6)
        : "r" (x8)
        : "memory"
    );

    wait_info_t info;
    info.status = (int)x0;
    info.tid = (uint32_t)x1;
    info.reason = (uint32_t)x2;
    info.exit_code = (int)x3;
    info.esr = x4;
    info.elr = x5;
    info.far = x6;
    return info;
}

#endif
