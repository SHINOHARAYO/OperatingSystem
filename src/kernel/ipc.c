#include "ipc.h"
#include "sched.h"
#include "memcap.h"
#include "orange_cat.h"

#define IPC_FAST_REPLY_TAG 0x80000000U
#define IPC_FAST_REPLY_GENERATION_MASK 0x7FFFFFFFU

static ipc_profile_t ipc_profiles[MAX_SCHED_CORES];

static uint32_t ipc_current_core_id(void) {
    uint64_t mpidr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static ipc_profile_t *ipc_current_profile(void) {
    uint32_t core_id = ipc_current_core_id();
    return &ipc_profiles[core_id < MAX_SCHED_CORES ? core_id : 0];
}

static void ipc_profile_add(uint64_t *counter, uint64_t value) {
    // Each core updates only its own profile; readers tolerate a slightly stale value.
    *counter += value;
}

static uint32_t ipc_reg_index(uint32_t word) {
    return 3 + word;
}

static uint32_t ipc_reply_reg_index(uint32_t word) {
    return 4 + word;
}

static uint32_t ipc_word_count(uint64_t len, uint32_t max_words) {
    uint64_t words = (len + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    return words > max_words ? max_words : (uint32_t)words;
}

static void clear_direct_caller_state(tcb_t *caller) {
    caller->ipc_target_tid = 0;
    caller->ipc_msg_flags = 0;
    caller->ipc_msg_len = 0;
    caller->ipc_next = NULL;
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

static int endpoint_tid_from_call_cap(tcb_t *task, uint32_t cap,
                                      uint32_t *tid_out) {
    ocap_t endpoint;
    if (!task || ocap_lookup(&task->caps, cap, OCAP_ENDPOINT,
                             OCAP_RIGHT_CALL, &endpoint) < 0) {
        return -1;
    }
    if (tid_out) *tid_out = endpoint.object_id;
    return 0;
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
    receiver->ipc_fast_receive = 0;
    sched_cancel_timeout(receiver);
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
    uint32_t words = ipc_word_count(len, IPC_INLINE_WORDS);
    for (uint32_t i = 0; i < words; i++) {
        recv_regs[ipc_reg_index(i)] = call_regs[ipc_reg_index(i)];
    }

    caller->ipc_target_tid = receiver->tid;
    caller->ipc_msg_flags = 0;
    caller->ipc_msg_len = len;
    caller->state = TASK_STATE_BLOCKED_ON_IPC_REPLY;
    receiver->ipc_target_tid = 0;
    receiver->ipc_fast_receive = 0;
    sched_cancel_timeout(receiver);
    return 0;
}

static uint32_t install_fast_reply(tcb_t *receiver, tcb_t *caller) {
    if (sched_task_hold(caller) < 0) return 0;
    uint32_t generation =
        (receiver->ipc_fast_reply_generation + 1) &
        IPC_FAST_REPLY_GENERATION_MASK;
    if (generation == 0) generation = 1;
    receiver->ipc_fast_reply_generation = generation;
    receiver->ipc_fast_reply_caller = caller;
    receiver->ipc_fast_reply_caller_tid = caller->tid;
    receiver->ipc_fast_reply_active = 1;
    return IPC_FAST_REPLY_TAG | generation;
}

static int deliver_direct_ipc_call(tcb_t *caller, tcb_t *receiver,
                                   uint64_t *recv_regs, uint64_t *call_regs,
                                   uint64_t len) {
    uint32_t reply_token = install_fast_reply(receiver, caller);
    if (reply_token == 0) return -1;
    recv_regs[0] = reply_token;
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
    receiver->ipc_fast_receive = 0;
    return 0;
}

static int direct_call_eligible(tcb_t *caller, tcb_t *receiver,
                                uint64_t flags, uint64_t len) {
    return caller && receiver && flags == 0 && len <= IPC_INLINE_BYTES &&
           caller->parent_tid > 1 && receiver->parent_tid > 1 &&
           receiver->ipc_fast_receive &&
           receiver->state == TASK_STATE_BLOCKED_ON_IPC_RECV &&
           caller->sched_core_id == receiver->sched_core_id &&
           !receiver->sched_queued && !receiver->timer_queued;
}

static tcb_t *cached_local_endpoint(tcb_t *caller, uint32_t endpoint_tid) {
    tcb_t *endpoint = caller->ipc_cached_endpoint;
    if (endpoint && caller->ipc_cached_endpoint_tid == endpoint_tid &&
        endpoint->tid == endpoint_tid &&
        endpoint->state != TASK_STATE_FREE &&
        endpoint->state != TASK_STATE_DEAD) {
        return endpoint;
    }

    endpoint = sched_find_task_local(endpoint_tid);
    caller->ipc_cached_endpoint = endpoint;
    caller->ipc_cached_endpoint_tid = endpoint ? endpoint_tid : 0;
    return endpoint;
}

int ipc_call_syscall(uint64_t *regs) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return -1;
    }

    ipc_profile_t *profile = ipc_current_profile();
    ipc_profile_add(&profile->calls, 1);
    current->ipc_profile_start = 0;
    uint32_t endpoint_tid = 0;
    if (endpoint_tid_from_call_cap(current, (uint32_t)regs[0],
                                   &endpoint_tid) < 0) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    uint64_t flags = regs[1];
    uint64_t len = regs[2];
    if ((flags & ~(IPC_FLAG_MEM | IPC_FLAG_CAP)) != 0 ||
        ((flags & IPC_FLAG_MEM) == 0 && len > IPC_INLINE_BYTES)) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    tcb_t *receiver = cached_local_endpoint(current, endpoint_tid);
    int receiver_held = 0;
    if (!receiver || !direct_call_eligible(current, receiver, flags, len)) {
        receiver = sched_task_get(endpoint_tid);
        receiver_held = 1;
    }
    if (!receiver) {
        regs[0] = (uint64_t)-1;
        return -1;
    }

    current->ipc_target_tid = (uint32_t)endpoint_tid;
    current->ipc_msg_flags = flags;
    current->ipc_msg_len = len;
    for (uint32_t i = 0; i < IPC_INLINE_WORDS; i++) {
        current->ipc_msg_payload[i] = regs[ipc_reg_index(i)];
    }

    int direct_deliver = 0;
    int direct_handoff = 0;
    int delivered = 0;
    uint64_t receiver_irq_flags = spin_lock_irqsave(&receiver->lock);
    if (receiver->state == TASK_STATE_BLOCKED_ON_IPC_RECV &&
        receiver->ipc_target_tid == (uint32_t)endpoint_tid) {
        uint64_t *recv_regs = sched_task_trap_frame(receiver);
        if (!receiver_held && direct_call_eligible(current, receiver, flags, len)) {
            delivered =
                deliver_direct_ipc_call(current, receiver, recv_regs, regs, len);
            direct_handoff = delivered == 0;
        } else {
            delivered = (flags == 0) ?
                deliver_fast_ipc_call(current, receiver, recv_regs, regs, len) :
                deliver_ipc_call(current, receiver, recv_regs);
        }
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
    spin_unlock_irqrestore(&receiver->lock, receiver_irq_flags);

    if (direct_deliver) {
        if (delivered < 0) {
            sched_clear_ipc_state(current);
            regs[0] = (uint64_t)-1;
            if (receiver_held) sched_task_put(receiver);
            return -1;
        }
        if (direct_handoff) {
            ipc_profile_add(&profile->direct_calls, 1);
            if (sched_ipc_direct_handoff(receiver) < 0) {
                sched_make_ready(receiver);
                sched_handoff_to_task(receiver);
            }
        } else {
            ipc_profile_add(&profile->slow_calls, 1);
            sched_make_ready(receiver);
            sched_handoff_to_task(receiver);
        }
        if (receiver_held) sched_task_put(receiver);
        return 0;
    }

    ipc_profile_add(&profile->slow_calls, 1);
    sched_reschedule();
    if (receiver_held) sched_task_put(receiver);
    return (int)regs[0];
}

static void ipc_recv_common(uint64_t *regs, uint64_t timeout_ms) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return;
    }

    int endpoint_tid = endpoint_tid_from_cap(current, (uint32_t)regs[0]);
    if (endpoint_tid < 0 || (uint32_t)endpoint_tid != current->tid) {
        regs[0] = (uint64_t)-1;
        return;
    }

    uint64_t current_irq_flags = spin_lock_irqsave(&current->lock);
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
        current->ipc_fast_receive = 0;
        current->ticks_remaining = 0;
        if (timeout_ms != 0) {
            sched_arm_timeout(current, timeout_ms);
        }
    }
    spin_unlock_irqrestore(&current->lock, current_irq_flags);

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

static int ipc_arm_receive(tcb_t *task) {
    if (!task) return -1;
    uint64_t irq_flags = spin_lock_irqsave(&task->lock);
    if (task->ipc_call_head) {
        spin_unlock_irqrestore(&task->lock, irq_flags);
        return -1;
    }
    task->state = TASK_STATE_BLOCKED_ON_IPC_RECV;
    task->ipc_target_tid = task->tid;
    task->ipc_fast_receive = 1;
    task->ticks_remaining = 0;
    spin_unlock_irqrestore(&task->lock, irq_flags);
    return 0;
}

void ipc_recv_syscall(uint64_t *regs) {
    ipc_recv_common(regs, 0);
}

void ipc_recv_timeout_syscall(uint64_t *regs, uint64_t timeout_ms) {
    if (!regs || timeout_ms == 0) {
        if (regs) regs[0] = (uint64_t)-1;
        return;
    }
    ipc_recv_common(regs, timeout_ms);
}

static int ipc_reply_common(uint64_t *regs, int receive_next) {
    tcb_t *current = sched_current_task();
    if (!regs || !current) {
        return -1;
    }

    uint32_t reply_cap = (uint32_t)regs[0];
    ipc_profile_t *profile = ipc_current_profile();
    ipc_profile_add(&profile->replies, 1);
    if ((reply_cap & IPC_FAST_REPLY_TAG) != 0) {
        uint32_t generation = reply_cap & IPC_FAST_REPLY_GENERATION_MASK;
        tcb_t *caller = current->ipc_fast_reply_caller;
        uint64_t flags = regs[2];
        uint64_t len = regs[3];
        if (!current->ipc_fast_reply_active || !caller ||
            current->ipc_fast_reply_caller_tid != caller->tid ||
            current->ipc_fast_reply_generation != generation ||
            caller->state != TASK_STATE_BLOCKED_ON_IPC_REPLY ||
            caller->ipc_target_tid != current->tid || flags != 0 ||
            len > IPC_REPLY_INLINE_WORDS * 8) {
            regs[0] = (uint64_t)-1;
            return -1;
        }

        uint64_t *caller_tf = sched_task_trap_frame(caller);
        caller_tf[0] = regs[1];
        caller_tf[1] = 0;
        caller_tf[2] = len;
        uint32_t words = ipc_word_count(len, IPC_REPLY_INLINE_WORDS);
        for (uint32_t i = 0; i < words; i++) {
            caller_tf[ipc_reg_index(i)] = regs[ipc_reply_reg_index(i)];
        }

        current->ipc_fast_reply_active = 0;
        current->ipc_fast_reply_caller = NULL;
        current->ipc_fast_reply_caller_tid = 0;
        clear_direct_caller_state(caller);
        regs[0] = 0;
        if (receive_next && ipc_arm_receive(current) == 0) {
            if (caller->sched_core_id == current->sched_core_id &&
                !caller->sched_queued && !caller->timer_queued &&
                sched_ipc_direct_handoff(caller) == 0) {
                sched_task_put(caller);
                return 1;
            }
            sched_make_ready(caller);
            sched_reschedule();
            sched_task_put(caller);
            return 1;
        }
        if (caller->sched_core_id == current->sched_core_id &&
            !caller->sched_queued && !caller->timer_queued &&
            sched_ipc_direct_reply_handoff(caller) == 0) {
            sched_task_put(caller);
            return 0;
        }
        sched_make_ready(caller);
        sched_handoff_to_task(caller);
        sched_task_put(caller);
        return 0;
    }

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
    if (receive_next && ipc_arm_receive(current) == 0) {
        regs[0] = 0;
        if (caller->sched_core_id == current->sched_core_id &&
            !caller->sched_queued && !caller->timer_queued &&
            sched_ipc_direct_handoff(caller) == 0) {
            sched_task_put(caller);
            return 1;
        }
        sched_make_ready(caller);
        sched_reschedule();
        sched_task_put(caller);
        return 1;
    }
    sched_make_ready(caller);
    regs[0] = 0;
    sched_handoff_to_task(caller);
    sched_task_put(caller);
    return 0;
}

int ipc_reply_syscall(uint64_t *regs) {
    return ipc_reply_common(regs, 0);
}

void ipc_reply_recv_syscall(uint64_t *regs) {
    if (!regs) return;
    int result = ipc_reply_common(regs, 1);
    if (result != 0) return;
    tcb_t *current = sched_current_task();
    if (current && current->state == TASK_STATE_RUNNING) {
        regs[0] = CAP_SELF;
        ipc_recv_common(regs, 0);
    }
}

void ipc_profile_syscall(uint64_t *regs) {
    if (!regs) return;
    ipc_profile_t total = {0};
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(total.counter_hz));
    for (uint32_t i = 0; i < MAX_SCHED_CORES; i++) {
        total.calls += __atomic_load_n(&ipc_profiles[i].calls, __ATOMIC_RELAXED);
        total.direct_calls +=
            __atomic_load_n(&ipc_profiles[i].direct_calls, __ATOMIC_RELAXED);
        total.slow_calls +=
            __atomic_load_n(&ipc_profiles[i].slow_calls, __ATOMIC_RELAXED);
        total.replies +=
            __atomic_load_n(&ipc_profiles[i].replies, __ATOMIC_RELAXED);
        total.cap_lookup_cycles +=
            __atomic_load_n(&ipc_profiles[i].cap_lookup_cycles, __ATOMIC_RELAXED);
        total.direct_roundtrip_cycles +=
            __atomic_load_n(&ipc_profiles[i].direct_roundtrip_cycles,
                            __ATOMIC_RELAXED);
        total.total_roundtrip_cycles +=
            __atomic_load_n(&ipc_profiles[i].total_roundtrip_cycles,
                            __ATOMIC_RELAXED);
    }
    regs[0] = total.calls;
    regs[1] = total.direct_calls;
    regs[2] = total.slow_calls;
    regs[3] = total.replies;
    regs[4] = total.cap_lookup_cycles;
    regs[5] = total.direct_roundtrip_cycles;
    regs[6] = total.total_roundtrip_cycles;
    regs[7] = total.counter_hz;
}

void ipc_task_died(uint32_t tid) {
    tcb_t *task = sched_find_task(tid);
    if (!task) return;
    if (task->ipc_fast_reply_active && task->ipc_fast_reply_caller) {
        sched_task_put(task->ipc_fast_reply_caller);
        task->ipc_fast_reply_active = 0;
        task->ipc_fast_reply_caller = NULL;
        task->ipc_fast_reply_caller_tid = 0;
    }
    sched_task_put(task);
}
