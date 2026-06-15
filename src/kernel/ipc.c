#include "ipc.h"
#include "sched.h"
#include "memcap.h"
#include "orange_cat.h"

static uint32_t ipc_reg_index(uint32_t word) {
    return 3 + word;
}

static uint32_t ipc_reply_reg_index(uint32_t word) {
    return 4 + word;
}

static int install_reply_cap(tcb_t *receiver, uint32_t caller_tid) {
    if (!receiver) {
        return -1;
    }
    return ocap_install(&receiver->caps, OCAP_REPLY, caller_tid, OCAP_RIGHT_REPLY, 0);
}

static int endpoint_tid_from_cap(tcb_t *task, uint32_t cap) {
    ocap_t endpoint;
    if (!task || ocap_lookup(&task->caps, cap, OCAP_ENDPOINT,
                             OCAP_RIGHT_CALL, &endpoint) < 0) {
        return -1;
    }

    uint32_t tid = endpoint.object_id;
    tcb_t *endpoint_task = sched_find_task(tid);
    if (!endpoint_task) {
        return -1;
    }
    sched_task_put(endpoint_task);
    return (int)tid;
}

static int copy_cap_to_task(tcb_t *src, tcb_t *dst, uint32_t src_cap) {
    uint32_t dst_slot = 0;
    if (!src || !dst ||
        ocap_copy(&src->caps, &dst->caps, src_cap,
                  OCAP_RIGHT_TRANSFER, &dst_slot) < 0) {
        return -1;
    }
    return (int)dst_slot;
}

static int prepare_ipc_payload(tcb_t *sender, tcb_t *receiver,
                               uint64_t flags, uint64_t len,
                               const uint64_t in[IPC_INLINE_WORDS],
                               uint64_t out[IPC_INLINE_WORDS]) {
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        out[i] = in[i];
    }

    if ((flags & IPC_FLAG_CAP) != 0) {
        int new_cap = copy_cap_to_task(sender, receiver, (uint32_t)in[0]);
        if (new_cap < 0) {
            return -1;
        }
        out[0] = (uint64_t)new_cap;
    }

    if ((flags & IPC_FLAG_MEM) != 0) {
        uint64_t remote_va = 0;
        if (memcap_import_ipc_memory(sender, receiver, in[0], in[1], in[2], in[3],
                                     &remote_va) < 0) {
            return -1;
        }
        out[0] = remote_va;
        out[1] = in[2];
        out[2] = in[3];
    } else if (len > IPC_INLINE_BYTES) {
        return -1;
    }

    return 0;
}

static void write_ipc_recv_regs(uint64_t *regs, uint32_t reply_cap,
                                uint32_t sender_tid, uint64_t len,
                                const uint64_t payload[IPC_INLINE_WORDS]) {
    regs[0] = reply_cap;
    regs[1] = sender_tid;
    regs[2] = len;
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        regs[ipc_reg_index(i)] = payload[i];
    }
}

static int deliver_ipc_call(tcb_t *caller, tcb_t *receiver, uint64_t *recv_regs) {
    uint64_t payload[IPC_INLINE_WORDS];
    if (prepare_ipc_payload(caller, receiver, caller->ipc_msg_flags,
                            caller->ipc_msg_len, caller->ipc_msg_payload,
                            payload) < 0) {
        return -1;
    }

    int reply_cap = install_reply_cap(receiver, caller->tid);
    if (reply_cap < 0) {
        return -1;
    }

    write_ipc_recv_regs(recv_regs, (uint32_t)reply_cap, caller->tid,
                        caller->ipc_msg_len, payload);
    caller->state = TASK_STATE_BLOCKED_ON_IPC_REPLY;
    receiver->ipc_target_tid = 0;
    return 0;
}

static int deliver_fast_ipc_call(tcb_t *caller, tcb_t *receiver,
                                 uint64_t *recv_regs, uint64_t *call_regs,
                                 uint64_t len) {
    int reply_cap = install_reply_cap(receiver, caller->tid);
    if (reply_cap < 0) {
        return -1;
    }

    recv_regs[0] = (uint64_t)reply_cap;
    recv_regs[1] = caller->tid;
    recv_regs[2] = len;
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        recv_regs[ipc_reg_index(i)] = call_regs[ipc_reg_index(i)];
    }

    caller->ipc_target_tid = receiver->tid;
    caller->ipc_msg_flags = 0;
    caller->ipc_msg_len = len;
    caller->state = TASK_STATE_BLOCKED_ON_IPC_REPLY;
    receiver->ipc_target_tid = 0;
    return 0;
}

int ipc_call_syscall(uint64_t *regs) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return -1;
    }

    int endpoint_tid = endpoint_tid_from_cap(current, (uint32_t)regs[0]);
    if (endpoint_tid < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }
    tcb_t *receiver = sched_find_task((uint32_t)endpoint_tid);
    if (!receiver) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    uint64_t flags = regs[1];
    uint64_t len = regs[2];
    if ((flags & ~(IPC_FLAG_MEM | IPC_FLAG_CAP)) != 0 ||
        ((flags & IPC_FLAG_MEM) == 0 && len > IPC_INLINE_BYTES)) {
        regs[0] = (uint64_t)-1;
        sched_task_put(receiver);
        return -1;
    }

    current->ipc_target_tid = (uint32_t)endpoint_tid;
    current->ipc_msg_flags = flags;
    current->ipc_msg_len = len;
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        current->ipc_msg_payload[i] = regs[ipc_reg_index(i)];
    }

    int direct_deliver = 0;
    int delivered = 0;
    spin_lock(&receiver->lock);
    if (receiver->state == TASK_STATE_BLOCKED_ON_IPC_RECV &&
        receiver->ipc_target_tid == (uint32_t)endpoint_tid) {
        uint64_t *recv_regs = sched_task_trap_frame(receiver);
        delivered = (flags == 0) ?
            deliver_fast_ipc_call(current, receiver, recv_regs, regs, len) :
            deliver_ipc_call(current, receiver, recv_regs);
        direct_deliver = 1;
    } else {
        current->state = TASK_STATE_BLOCKED_ON_IPC_CALL;
        current->ticks_remaining = 0;
        current->ipc_next = NULL;
        if (receiver->ipc_call_tail) {
            receiver->ipc_call_tail->ipc_next = current;
        } else {
            receiver->ipc_call_head = current;
        }
        receiver->ipc_call_tail = current;
    }
    spin_unlock(&receiver->lock);

    if (direct_deliver) {
        if (delivered < 0) {
            sched_clear_ipc_state(current);
            regs[0] = (uint64_t)-1;
            sched_task_put(receiver);
            return -1;
        }
        sched_make_ready(receiver);
        sched_handoff_to_task(receiver);
        sched_task_put(receiver);
        return 0;
    }

    sched_reschedule();
    sched_task_put(receiver);
    return (int)regs[0];
}

void ipc_recv_syscall(uint64_t *regs) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return;
    }

    int endpoint_tid = endpoint_tid_from_cap(current, (uint32_t)regs[0]);
    if (endpoint_tid < 0 || (uint32_t)endpoint_tid != current->tid) {
        regs[0] = (uint64_t)-1;
        return;
    }

    spin_lock(&current->lock);
    tcb_t *caller = current->ipc_call_head;
    if (caller) {
        current->ipc_call_head = caller->ipc_next;
        if (current->ipc_call_tail == caller) {
            current->ipc_call_tail = NULL;
        }
        caller->ipc_next = NULL;
    } else {
        current->state = TASK_STATE_BLOCKED_ON_IPC_RECV;
        current->ipc_target_tid = current->tid;
        current->ticks_remaining = 0;
    }
    spin_unlock(&current->lock);

    if (caller) {
        if (caller->state == TASK_STATE_BLOCKED_ON_IPC_CALL &&
            caller->ipc_target_tid == current->tid) {
            if (deliver_ipc_call(caller, current, regs) < 0) {
                uint64_t *caller_tf = sched_task_trap_frame(caller);
                caller_tf[0] = (uint64_t)-1;
                sched_clear_ipc_state(caller);
                sched_make_ready(caller);
                regs[0] = (uint64_t)-1;
            }
        } else {
            regs[0] = (uint64_t)-1;
        }
        return;
    }

    sched_reschedule();
}

int ipc_reply_syscall(uint64_t *regs) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return -1;
    }

    uint32_t reply_cap = (uint32_t)regs[0];
    ocap_t reply;
    if (ocap_lookup(&current->caps, reply_cap, OCAP_REPLY,
                    OCAP_RIGHT_REPLY, &reply) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    tcb_t *caller = sched_find_task(reply.object_id);
    if (!caller || caller->state != TASK_STATE_BLOCKED_ON_IPC_REPLY) {
        ocap_revoke_slot(&current->caps, reply_cap);
        regs[0] = (uint64_t)-1;
        if (caller) {
            sched_task_put(caller);
        }
        return -1;
    }

    uint64_t flags = regs[2];
    uint64_t len = regs[3];
    uint64_t in[IPC_INLINE_WORDS];
    uint64_t out[IPC_INLINE_WORDS];
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        in[i] = (i < IPC_REPLY_INLINE_WORDS) ? regs[ipc_reply_reg_index(i)] : 0;
    }

    if ((flags & ~(IPC_FLAG_MEM | IPC_FLAG_CAP)) != 0 ||
        ((flags & IPC_FLAG_MEM) == 0 && len > IPC_REPLY_INLINE_WORDS * 8) ||
        (flags != 0 && prepare_ipc_payload(current, caller, flags, len, in, out) < 0)) {
        regs[0] = (uint64_t)-1;
        sched_task_put(caller);
        return -1;
    }

    uint64_t *caller_tf = sched_task_trap_frame(caller);
    caller_tf[0] = regs[1];
    caller_tf[1] = flags;
    caller_tf[2] = len;
    if (flags == 0) {
        for (uint32_t i = 0; i < IPC_REPLY_INLINE_WORDS; i++) {
            caller_tf[ipc_reg_index(i)] = regs[ipc_reply_reg_index(i)];
        }
        caller_tf[ipc_reg_index(IPC_INLINE_WORDS - 1)] = 0;
    } else {
        for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
            caller_tf[ipc_reg_index(i)] = out[i];
        }
    }

    ocap_revoke_slot(&current->caps, reply_cap);
    sched_clear_ipc_state(caller);
    sched_make_ready(caller);
    regs[0] = 0;
    sched_handoff_to_task(caller);
    sched_task_put(caller);
    return 0;
}
