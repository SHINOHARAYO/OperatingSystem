#include "lib.h"
#include "ipc_proto.h"

void _start(void) {
    ipc_msg_t setup = sys_ipc_recv(CAP_SELF);
    uint32_t endpoint_cap = (uint32_t)setup.payload[IPC_CAP_WORD_CAP];

    uint64_t payload[IPC_INLINE_WORDS] = {0xCAFE, 41};
    ipc_msg_t reply = sys_ipc_call(endpoint_cap, 0, 16, payload);
    int ok = reply.status == 0 &&
             reply.payload[0] == ~payload[0] &&
             reply.payload[1] == payload[1] + 1;

    uint64_t out[IPC_REPLY_INLINE_WORDS] = {ok ? 1ULL : 0ULL, reply.payload[1]};
    sys_ipc_reply(setup.reply_cap, ok ? 0 : -1, 0, 16, out);
    sys_exit(ok ? 0 : 1);
}
