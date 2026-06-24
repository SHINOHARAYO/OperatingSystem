#include "pipe.h"
#include "orange_cat.h"
#include "sched.h"
#include "spinlock.h"
#include "usercopy.h"
#include <stddef.h>

#define PIPE_MAX 64
#define PIPE_BUFFER_SIZE 4096
#define PIPE_COPY_CHUNK 256
#define PIPE_FLAG_READ_END  1ULL
#define PIPE_FLAG_WRITE_END 2ULL

typedef struct {
    spinlock_t lock;
    uint8_t used;
    uint32_t read_refs;
    uint32_t write_refs;
    uint32_t head;
    uint32_t count;
    uint8_t data[PIPE_BUFFER_SIZE];
} kernel_pipe_t;

static kernel_pipe_t pipes[PIPE_MAX];
static spinlock_t pipe_table_lock;

static void zero_bytes(void *ptr, size_t size) {
    uint8_t *bytes = (uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) bytes[i] = 0;
}

static kernel_pipe_t *pipe_from_cap(const ocap_t *cap) {
    if (!cap || cap->object_id >= PIPE_MAX) return NULL;
    kernel_pipe_t *pipe = &pipes[cap->object_id];
    return pipe->used ? pipe : NULL;
}

static void pipe_release_cap(const ocap_t *cap) {
    if (!cap || cap->type != OCAP_PIPE || cap->object_id >= PIPE_MAX) return;
    kernel_pipe_t *pipe = &pipes[cap->object_id];
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    if (pipe->used) {
        if ((cap->flags & PIPE_FLAG_READ_END) && pipe->read_refs) pipe->read_refs--;
        if ((cap->flags & PIPE_FLAG_WRITE_END) && pipe->write_refs) pipe->write_refs--;
        if (pipe->read_refs == 0 && pipe->write_refs == 0) {
            pipe->used = 0;
            pipe->head = 0;
            pipe->count = 0;
        }
    }
    spin_unlock_irqrestore(&pipe->lock, flags);
}

static int pipe_acquire_cap(const ocap_t *cap) {
    if (!cap || cap->type != OCAP_PIPE || cap->object_id >= PIPE_MAX) return -1;
    kernel_pipe_t *pipe = &pipes[cap->object_id];
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    if (!pipe->used) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return -1;
    }
    if (cap->flags & PIPE_FLAG_READ_END) pipe->read_refs++;
    if (cap->flags & PIPE_FLAG_WRITE_END) pipe->write_refs++;
    spin_unlock_irqrestore(&pipe->lock, flags);
    return 0;
}

void pipe_init(void) {
    zero_bytes(pipes, sizeof(pipes));
    pipe_table_lock.value = 0;
    (void)ocap_set_type_release_hook(OCAP_PIPE, pipe_release_cap);
    (void)ocap_set_type_acquire_hook(OCAP_PIPE, pipe_acquire_cap);
}

void pipe_create_syscall(uint64_t *regs) {
    tcb_t *task = sched_current_task();
    if (!regs || !task) return;
    regs[0] = (uint64_t)-1;
    regs[1] = (uint64_t)-1;

    uint64_t table_flags = spin_lock_irqsave(&pipe_table_lock);
    uint32_t index;
    for (index = 0; index < PIPE_MAX && pipes[index].used; index++) {}
    if (index == PIPE_MAX) {
        spin_unlock_irqrestore(&pipe_table_lock, table_flags);
        return;
    }
    kernel_pipe_t *pipe = &pipes[index];
    pipe->used = 1;
    pipe->read_refs = 0;
    pipe->write_refs = 0;
    pipe->head = 0;
    pipe->count = 0;
    spin_unlock_irqrestore(&pipe_table_lock, table_flags);

    int read_cap = ocap_install(&task->caps, OCAP_PIPE, index,
                                OCAP_RIGHT_READ | OCAP_RIGHT_TRANSFER,
                                PIPE_FLAG_READ_END);
    if (read_cap < 0) {
        pipe->used = 0;
        return;
    }
    pipe->read_refs = 1;
    int write_cap = ocap_install(&task->caps, OCAP_PIPE, index,
                                 OCAP_RIGHT_WRITE | OCAP_RIGHT_TRANSFER,
                                 PIPE_FLAG_WRITE_END);
    if (write_cap < 0) {
        ocap_revoke_slot(&task->caps, (uint32_t)read_cap);
        return;
    }
    pipe->write_refs = 1;
    regs[0] = (uint64_t)read_cap;
    regs[1] = (uint64_t)write_cap;
}

void pipe_read_syscall(uint64_t *regs, uint32_t cap_slot, uint64_t buffer,
                       uint64_t count) {
    tcb_t *task = sched_current_task();
    if (!regs || !task) return;
    regs[0] = (uint64_t)-1;
    if (count == 0) { regs[0] = 0; return; }
    ocap_t cap;
    if (!buffer || ocap_lookup(&task->caps, cap_slot, OCAP_PIPE,
                               OCAP_RIGHT_READ, &cap) < 0) return;
    kernel_pipe_t *pipe = pipe_from_cap(&cap);
    if (!pipe) return;

    uint8_t chunk[PIPE_COPY_CHUNK];
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    if (pipe->count == 0) {
        regs[0] = pipe->write_refs == 0 ? 0 : (uint64_t)PIPE_WOULD_BLOCK;
        spin_unlock_irqrestore(&pipe->lock, flags);
        return;
    }
    uint32_t amount = pipe->count;
    if (amount > count) amount = (uint32_t)count;
    if (amount > sizeof(chunk)) amount = sizeof(chunk);
    for (uint32_t i = 0; i < amount; i++)
        chunk[i] = pipe->data[(pipe->head + i) % PIPE_BUFFER_SIZE];
    pipe->head = (pipe->head + amount) % PIPE_BUFFER_SIZE;
    pipe->count -= amount;
    spin_unlock_irqrestore(&pipe->lock, flags);
    regs[0] = copy_to_user(buffer, chunk, amount) < 0 ? (uint64_t)-1 : amount;
}

void pipe_write_syscall(uint64_t *regs, uint32_t cap_slot, uint64_t buffer,
                        uint64_t count) {
    tcb_t *task = sched_current_task();
    if (!regs || !task) return;
    regs[0] = (uint64_t)-1;
    if (count == 0) { regs[0] = 0; return; }
    ocap_t cap;
    if (!buffer || ocap_lookup(&task->caps, cap_slot, OCAP_PIPE,
                               OCAP_RIGHT_WRITE, &cap) < 0) return;
    kernel_pipe_t *pipe = pipe_from_cap(&cap);
    if (!pipe) return;
    uint8_t chunk[PIPE_COPY_CHUNK];
    uint32_t amount = count > sizeof(chunk) ? sizeof(chunk) : (uint32_t)count;
    if (copy_from_user(chunk, buffer, amount) < 0) return;

    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    if (pipe->read_refs == 0) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return;
    }
    uint32_t space = PIPE_BUFFER_SIZE - pipe->count;
    if (space == 0) {
        regs[0] = (uint64_t)PIPE_WOULD_BLOCK;
        spin_unlock_irqrestore(&pipe->lock, flags);
        return;
    }
    if (amount > space) amount = space;
    uint32_t tail = (pipe->head + pipe->count) % PIPE_BUFFER_SIZE;
    for (uint32_t i = 0; i < amount; i++)
        pipe->data[(tail + i) % PIPE_BUFFER_SIZE] = chunk[i];
    pipe->count += amount;
    spin_unlock_irqrestore(&pipe->lock, flags);
    regs[0] = amount;
}

void pipe_close_syscall(uint64_t *regs, uint32_t cap) {
    tcb_t *task = sched_current_task();
    if (!regs || !task || ocap_lookup(&task->caps, cap, OCAP_PIPE, 0, NULL) < 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return;
    }
    ocap_revoke_slot(&task->caps, cap);
    regs[0] = 0;
}

void pipe_dup_syscall(uint64_t *regs, uint32_t cap_slot) {
    tcb_t *task = sched_current_task();
    if (!regs || !task) return;
    regs[0] = (uint64_t)-1;
    ocap_t cap;
    if (ocap_lookup(&task->caps, cap_slot, OCAP_PIPE, 0, &cap) < 0) return;
    kernel_pipe_t *pipe = pipe_from_cap(&cap);
    if (!pipe) return;
    uint32_t new_cap = 0;
    if (ocap_copy(&task->caps, &task->caps, cap_slot, 0, &new_cap) < 0) return;
    regs[0] = new_cap;
}

int pipe_inherit_cap(tcb_t *parent, tcb_t *child, uint32_t parent_cap,
                     uint32_t child_slot) {
    if (!parent || !child || child_slot < CAP_STDIN || child_slot > CAP_STDERR)
        return -1;
    ocap_t cap;
    if (ocap_lookup(&parent->caps, parent_cap, OCAP_PIPE, 0, &cap) < 0)
        return -1;
    kernel_pipe_t *pipe = pipe_from_cap(&cap);
    if (!pipe) return -1;
    return ocap_clone_at(&parent->caps, &child->caps, parent_cap, child_slot, 0);
}
