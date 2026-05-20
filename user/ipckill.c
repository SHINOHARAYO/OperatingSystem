#include "lib.h"
#include "ipc_proto.h"

#define IPCKILL_SERVER_HOLD        1
#define IPCKILL_SERVER_DELAY_REPLY 2
#define IPCKILL_CALLER_CALL        3
#define IPCKILL_RECV_WAIT          4

static void reply_ack(uint32_t reply_cap) {
    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {1};
    sys_ipc_reply(reply_cap, 0, 0, 8, reply);
}

void _start(void) {
    ipc_msg_t setup = sys_ipc_recv(CAP_SELF);
    uint64_t mode = setup.payload[IPC_WORD_OP];
    if (setup.payload[IPC_CAP_WORD_OP] == IPCKILL_CALLER_CALL) {
        mode = IPCKILL_CALLER_CALL;
    }

    if (mode == IPCKILL_SERVER_HOLD) {
        reply_ack(setup.reply_cap);
        ipc_msg_t request = sys_ipc_recv(CAP_SELF);
        (void)request;
        while (1) {
            sys_sleep(1000);
        }
    }

    if (mode == IPCKILL_SERVER_DELAY_REPLY) {
        reply_ack(setup.reply_cap);
        ipc_msg_t request = sys_ipc_recv(CAP_SELF);
        sys_sleep(3000);
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
        int status = sys_ipc_reply(request.reply_cap, 0, 0, 8, reply);
        sys_exit(status < 0 ? 0 : 1);
    }

    if (mode == IPCKILL_CALLER_CALL) {
        uint32_t server_cap = (uint32_t)setup.payload[IPC_CAP_WORD_CAP];
        reply_ack(setup.reply_cap);
        uint64_t payload[IPC_INLINE_WORDS] = {0x51};
        ipc_msg_t reply = sys_ipc_call(server_cap, 0, 8, payload);
        sys_exit(reply.status < 0 ? 0 : 1);
    }

    if (mode == IPCKILL_RECV_WAIT) {
        reply_ack(setup.reply_cap);
        (void)sys_ipc_recv(CAP_SELF);
        sys_exit(1);
    }

    uint64_t fail[IPC_REPLY_INLINE_WORDS] = {0};
    sys_ipc_reply(setup.reply_cap, -1, 0, 8, fail);
    sys_exit(1);
}
