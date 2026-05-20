#include "lib.h"

void _start(void) {
    ipc_msg_t lend = sys_ipc_recv(CAP_SELF);
    char *buf = (char *)lend.payload[0];
    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {buf ? 1ULL : 0ULL, (uint64_t)buf};
    sys_ipc_reply(lend.reply_cap, buf ? 0 : -1, 0, 16, reply);

    ipc_msg_t after_revoke = sys_ipc_recv(CAP_SELF);
    char *revoked = (char *)after_revoke.payload[0];
    revoked[0] = '!';

    uint64_t unexpected[IPC_REPLY_INLINE_WORDS] = {0};
    sys_ipc_reply(after_revoke.reply_cap, -1, 0, 8, unexpected);
    sys_exit(1);
}
