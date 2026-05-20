#ifndef _NS_PROTO_H_
#define _NS_PROTO_H_

#include "ipc_proto.h"

#define NS_TID 1

#define NS_REQ_REGISTER 1
#define NS_REQ_RESOLVE  2

static inline uint64_t ns_pack_name(const char *name) {
    uint64_t packed = 0;
    char *dst = (char *)&packed;
    int i = 0;
    for (; i < 8 && name[i]; i++) {
        dst[i] = name[i];
    }
    return packed;
}

static inline int ns_register(const char *name) {
    uint64_t packed = ns_pack_name(name);
    uint64_t payload[IPC_INLINE_WORDS] = {0};
    payload[IPC_CAP_WORD_CAP] = CAP_SELF;
    payload[IPC_CAP_WORD_OP] = NS_REQ_REGISTER;
    payload[IPC_CAP_WORD_ARG0] = packed;
    ipc_msg_t reply = sys_ipc_call(CAP_NS, IPC_FLAG_CAP, 24, payload);
    return reply.status < 0 ? -1 : (int)reply.payload[0];
}

static inline int ns_resolve(const char *name) {
    uint64_t packed = ns_pack_name(name);
    uint64_t payload[IPC_INLINE_WORDS] = {0};
    payload[IPC_WORD_OP] = NS_REQ_RESOLVE;
    payload[IPC_WORD_ARG0] = packed;
    ipc_msg_t reply = sys_ipc_call(CAP_NS, 0, 16, payload);
    return reply.status < 0 ? -1 : (int)reply.payload[0];
}

#endif
