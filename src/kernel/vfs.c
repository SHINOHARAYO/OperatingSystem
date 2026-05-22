#include "vfs.h"
#include "sched.h"
#include "orange_cat.h"
#include "frame.h"
#include "vmm.h"
#include "vma.h"
#include "mmu.h"

#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

static void zero_page(uint64_t paddr) {
    uint8_t *p = (uint8_t *)(paddr & PTE_ADDR_MASK);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        p[i] = 0;
    }
}

void vfs_clear_task_state(tcb_t *task) {
    if (!task) {
        return;
    }

    for (uint32_t i = 0; i < MAX_VFS_ROUTES; i++) {
        task->vfs_ids[i] = 0;
        task->vfs_tids[i] = 0;
    }
    task->vfs_active_client_tid = 0;
    task->vfs_active_id = 0;
    task->vfs_reply_tid = 0;
    task->vfs_args[0] = 0;
    task->vfs_args[1] = 0;
    task->vfs_args[2] = 0;
    task->vfs_next = 0;
    task->vfs_call_head = 0;
    task->vfs_call_tail = 0;
}

static int find_route(tcb_t *task, uint32_t vfs_id) {
    if (!task || vfs_id == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < MAX_VFS_ROUTES; i++) {
        if (task->vfs_ids[i] == vfs_id && task->vfs_tids[i] != 0) {
            return (int)i;
        }
    }
    return -1;
}

static void enqueue_vfs_caller(tcb_t *fs, tcb_t *caller) {
    caller->vfs_next = 0;
    if (fs->vfs_call_tail) {
        fs->vfs_call_tail->vfs_next = caller;
    } else {
        fs->vfs_call_head = caller;
    }
    fs->vfs_call_tail = caller;
}

static tcb_t *dequeue_vfs_caller(tcb_t *fs) {
    if (!fs || !fs->vfs_call_head) {
        return 0;
    }

    tcb_t *caller = fs->vfs_call_head;
    fs->vfs_call_head = caller->vfs_next;
    if (fs->vfs_call_tail == caller) {
        fs->vfs_call_tail = 0;
    }
    caller->vfs_next = 0;
    return caller;
}

static void unlink_vfs_caller(tcb_t *caller) {
    if (!caller || caller->vfs_reply_tid == 0) {
        return;
    }

    tcb_t *fs = sched_find_task(caller->vfs_reply_tid);
    if (!fs) {
        caller->vfs_next = 0;
        return;
    }

    tcb_t *prev = 0;
    tcb_t *cur = fs->vfs_call_head;
    while (cur) {
        if (cur == caller) {
            if (prev) {
                prev->vfs_next = cur->vfs_next;
            } else {
                fs->vfs_call_head = cur->vfs_next;
            }
            if (fs->vfs_call_tail == cur) {
                fs->vfs_call_tail = prev;
            }
            cur->vfs_next = 0;
            return;
        }
        prev = cur;
        cur = cur->vfs_next;
    }
}

static void deliver_vfs_call(tcb_t *caller, tcb_t *fs, uint64_t *fs_regs) {
    fs_regs[0] = caller->vfs_active_id;
    fs_regs[1] = caller->vfs_args[0];
    fs_regs[2] = caller->vfs_args[1];
    fs_regs[3] = caller->vfs_args[2];
    fs_regs[4] = caller->tid;

    fs->vfs_active_client_tid = caller->tid;
    fs->vfs_active_id = caller->vfs_active_id;
    caller->state = TASK_STATE_BLOCKED_ON_VFS_CALL;
    caller->ticks_remaining = 0;
}

void vfs_bind_syscall(uint64_t *regs, uint32_t vfs_id, uint32_t endpoint_cap) {
    tcb_t *current = sched_current_task();
    if (!regs || !current || vfs_id == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return;
    }

    ocap_t cap;
    if (ocap_lookup(&current->caps, endpoint_cap, OCAP_ENDPOINT,
                    OCAP_RIGHT_CALL, &cap) < 0 ||
        !sched_find_task(cap.object_id)) {
        regs[0] = (uint64_t)-1;
        return;
    }

    int slot = find_route(current, vfs_id);
    if (slot < 0) {
        for (uint32_t i = 0; i < MAX_VFS_ROUTES; i++) {
            if (current->vfs_ids[i] == 0) {
                slot = (int)i;
                break;
            }
        }
    }

    if (slot < 0) {
        regs[0] = (uint64_t)-1;
        return;
    }

    current->vfs_ids[slot] = vfs_id;
    current->vfs_tids[slot] = cap.object_id;
    regs[0] = 0;
}

void vfs_recv_syscall(uint64_t *regs) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return;
    }

    tcb_t *caller = dequeue_vfs_caller(current);
    if (caller && caller->state == TASK_STATE_BLOCKED_ON_VFS_CALL &&
        caller->vfs_reply_tid == current->tid) {
        deliver_vfs_call(caller, current, regs);
        return;
    }

    current->state = TASK_STATE_BLOCKED_ON_VFS_RECV;
    current->ticks_remaining = 0;
    sched_reschedule();
}

void vfs_call_syscall(uint64_t *regs) {
    tcb_t *caller = sched_current_task();
    if (!regs || !caller) {
        return;
    }

    uint32_t vfs_id = (uint32_t)regs[0];
    int route = find_route(caller, vfs_id);
    if (route < 0) {
        regs[0] = (uint64_t)-1;
        return;
    }

    tcb_t *fs = sched_find_task(caller->vfs_tids[route]);
    if (!fs || fs->vfs_active_client_tid != 0) {
        regs[0] = (uint64_t)-1;
        return;
    }

    caller->vfs_active_id = vfs_id;
    caller->vfs_args[0] = regs[1];
    caller->vfs_args[1] = regs[2];
    caller->vfs_args[2] = regs[3];
    caller->vfs_reply_tid = fs->tid;
    caller->state = TASK_STATE_BLOCKED_ON_VFS_CALL;
    caller->ticks_remaining = 0;

    if (fs->state == TASK_STATE_BLOCKED_ON_VFS_RECV) {
        uint64_t *fs_tf = sched_task_trap_frame(fs);
        deliver_vfs_call(caller, fs, fs_tf);
        sched_make_ready(fs);
        sched_handoff_to_task(fs);
        return;
    }

    enqueue_vfs_caller(fs, caller);
    sched_reschedule();
}

void vfs_reply_syscall(uint64_t *regs) {
    tcb_t *fs = sched_current_task();
    if (!regs || !fs || fs->vfs_active_client_tid == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return;
    }

    tcb_t *client = sched_find_task(fs->vfs_active_client_tid);
    if (!client || client->state != TASK_STATE_BLOCKED_ON_VFS_CALL ||
        client->vfs_reply_tid != fs->tid) {
        fs->vfs_active_client_tid = 0;
        fs->vfs_active_id = 0;
        regs[0] = (uint64_t)-1;
        return;
    }

    uint64_t *client_tf = sched_task_trap_frame(client);
    client_tf[0] = regs[0];
    client_tf[1] = regs[1];
    client_tf[2] = regs[2];
    client_tf[3] = regs[3];

    client->vfs_reply_tid = 0;
    client->vfs_active_id = 0;
    client->vfs_args[0] = 0;
    client->vfs_args[1] = 0;
    client->vfs_args[2] = 0;
    fs->vfs_active_client_tid = 0;
    fs->vfs_active_id = 0;

    sched_make_ready(client);
    regs[0] = 0;
    sched_handoff_to_task(client);
}

static int range_has_no_mappings(tcb_t *task, uint64_t start, uint64_t size) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t pa = 0;
        uint64_t entry = 0;
        if (vmm_query_page(task->pgd, start + off, &pa, &entry) == 0 &&
            (entry & PTE_USER) != 0) {
            return -1;
        }
    }
    return 0;
}

static int range_has_user_write_vmas(tcb_t *task, uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vma_t *vma = vma_find(&task->vm, va);
        if (!vma || va < vma->start || va >= vma->end ||
            (vma->flags & VMA_GUARD) ||
            (vma->flags & (VMA_USER | VMA_READ | VMA_WRITE)) !=
                (VMA_USER | VMA_READ | VMA_WRITE)) {
            return -1;
        }
    }
    return 0;
}

static uint64_t find_fs_shared_va(tcb_t *fs, uint64_t size) {
    uint64_t va = fs->user_shared_pointer;
    if (va < USER_SHARED_BASE) {
        va = USER_SHARED_BASE;
    }

    while (va <= VMA_USER_LIMIT && size <= VMA_USER_LIMIT - va) {
        if (vma_range_is_free(&fs->vm, va, va + size)) {
            return va;
        }
        va += size;
    }
    return 0;
}

void vfs_inject_syscall(uint64_t *regs, uint32_t client_tid,
                        uint64_t client_va, uint64_t page_count) {
    tcb_t *fs = sched_current_task();
    if (!regs || !fs || !fs->pgd || client_tid == 0 ||
        fs->vfs_active_client_tid != client_tid ||
        (client_va & (PAGE_SIZE - 1)) != 0 ||
        page_count == 0 || page_count > VFS_MAX_INJECT_PAGES ||
        page_count > UINT64_MAX / PAGE_SIZE) {
        if (regs) regs[0] = 0;
        return;
    }

    tcb_t *client = sched_find_task(client_tid);
    uint64_t size = page_count * PAGE_SIZE;
    if (!client || !client->pgd || size > VMA_USER_LIMIT ||
        client_va > VMA_USER_LIMIT || size > VMA_USER_LIMIT - client_va ||
        range_has_no_mappings(client, client_va, size) < 0) {
        regs[0] = 0;
        return;
    }

    int client_vma_added = 0;
    if (vma_range_is_free(&client->vm, client_va, client_va + size)) {
        if (vma_add_ex(&client->vm, client_va, client_va + size,
                       VMA_USER | VMA_READ | VMA_WRITE | VMA_MMAP,
                       0, size, 0, VMA_BACKING_ANON, 0,
                       page_count, page_count) < 0) {
            regs[0] = 0;
            return;
        }
        client_vma_added = 1;
    } else if (range_has_user_write_vmas(client, client_va, size) < 0) {
        regs[0] = 0;
        return;
    }

    uint64_t fs_va = find_fs_shared_va(fs, size);
    if (!fs_va ||
        vma_add_ex(&fs->vm, fs_va, fs_va + size,
                   VMA_USER | VMA_READ | VMA_WRITE | VMA_MMAP,
                   0, size, 0, VMA_BACKING_ANON, 0,
                   page_count, page_count) < 0) {
        if (client_vma_added) {
            vma_remove(&client->vm, client_va, client_va + size);
        }
        regs[0] = 0;
        return;
    }

    uint64_t mapped = 0;
    for (; mapped < size; mapped += PAGE_SIZE) {
        frame_t *frame = frame_alloc(client->tid, FRAME_FLAG_USER | FRAME_FLAG_SHARED);
        if (!frame) {
            break;
        }
        zero_page(frame->paddr);

        if (vmm_map_page_asid(client->pgd, client->asid,
                              client_va + mapped, frame->paddr,
                              VMM_FLAG_USER_RW | VMM_FLAG_OWNED) < 0) {
            frame_unref(frame->paddr);
            break;
        }

        if (frame_ref(frame->paddr) < 0) {
            vmm_unmap_page_asid(client->pgd, client->asid, client_va + mapped);
            break;
        }

        if (vmm_map_page_asid(fs->pgd, fs->asid,
                              fs_va + mapped, frame->paddr,
                              VMM_FLAG_USER_RW | VMM_FLAG_OWNED) < 0) {
            frame_unref(frame->paddr);
            vmm_unmap_page_asid(client->pgd, client->asid, client_va + mapped);
            break;
        }
    }

    if (mapped != size) {
        for (uint64_t off = 0; off < mapped; off += PAGE_SIZE) {
            vmm_unmap_page_asid(client->pgd, client->asid, client_va + off);
            vmm_unmap_page_asid(fs->pgd, fs->asid, fs_va + off);
        }
        if (client_vma_added) {
            vma_remove(&client->vm, client_va, client_va + size);
        }
        vma_remove(&fs->vm, fs_va, fs_va + size);
        regs[0] = 0;
        return;
    }

    fs->user_shared_pointer = fs_va + size;
    regs[0] = fs_va;
    regs[1] = client_va;
    regs[2] = size;
}

void vfs_task_died(uint32_t tid) {
    if (tid == 0) {
        return;
    }

    uint32_t capacity = sched_task_capacity();
    for (uint32_t i = 0; i < capacity; i++) {
        tcb_t *task = sched_task_at(i);
        if (!task) {
            continue;
        }

        for (uint32_t r = 0; r < MAX_VFS_ROUTES; r++) {
            if (task->vfs_tids[r] == tid) {
                task->vfs_ids[r] = 0;
                task->vfs_tids[r] = 0;
            }
        }

        if (task->tid == tid) {
            unlink_vfs_caller(task);
        }

        if (task->vfs_active_client_tid == tid) {
            task->vfs_active_client_tid = 0;
            task->vfs_active_id = 0;
        }

        if (task->state == TASK_STATE_BLOCKED_ON_VFS_CALL &&
            task->vfs_reply_tid == tid) {
            uint64_t *tf = sched_task_trap_frame(task);
            tf[0] = (uint64_t)-1;
            tf[1] = 0;
            tf[2] = 0;
            tf[3] = 0;
            task->vfs_reply_tid = 0;
            task->vfs_active_id = 0;
            task->vfs_args[0] = 0;
            task->vfs_args[1] = 0;
            task->vfs_args[2] = 0;
            sched_make_ready(task);
        }

        if (task->tid == tid) {
            while (task->vfs_call_head) {
                tcb_t *caller = dequeue_vfs_caller(task);
                if (caller && caller->state == TASK_STATE_BLOCKED_ON_VFS_CALL) {
                    uint64_t *tf = sched_task_trap_frame(caller);
                    tf[0] = (uint64_t)-1;
                    tf[1] = 0;
                    tf[2] = 0;
                    tf[3] = 0;
                    caller->vfs_reply_tid = 0;
                    caller->vfs_active_id = 0;
                    sched_make_ready(caller);
                }
            }
        }
    }
}
