#include "lib.h"
#include "ipc_proto.h"

#define IPCCAP_MODE_SERVER 1
#define IPCCAP_MODE_CLIENT 2

void _start(void) {
    ipc_msg_t setup = sys_ipc_recv(CAP_SELF);
    uint64_t mode = setup.payload[IPC_CAP_WORD_OP];

    if (mode == IPCCAP_MODE_SERVER) {
        uint64_t ready[IPC_REPLY_INLINE_WORDS] = {1};
        sys_ipc_reply(setup.reply_cap, 0, 0, 8, ready);

        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        int ok = msg.status >= 0 &&
                 msg.payload[0] == 0xCAFE &&
                 msg.payload[1] == 41;
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {~msg.payload[0], msg.payload[1] + 1};
        sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 16, reply);
        sys_exit(ok ? 0 : 1);
    }

    if (mode != IPCCAP_MODE_CLIENT) {
        uint64_t out[IPC_REPLY_INLINE_WORDS] = {0};
        sys_ipc_reply(setup.reply_cap, -1, 0, 8, out);
        sys_exit(1);
    }

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
