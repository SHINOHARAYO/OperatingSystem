#include "lib.h"

#define IPC_STOP 0xFFFFFFFFFFFFFFFFULL
#define IPC_PING 0x51EEDULL
#define IPC_PONG 0xC0FFEEULL

void _start(void) {
    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        uint64_t op = msg.payload[0];
        if (op == IPC_STOP) {
            sys_ipc_reply(msg.reply_cap, 0, 0, 0, 0);
            sys_exit(0);
        }

        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
            op == IPC_PING ? IPC_PONG : op + 1,
            msg.len
        };
        sys_ipc_reply(msg.reply_cap, 0, 0, 16, reply);
    }
}
