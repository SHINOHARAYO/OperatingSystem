#include "lib.h"

void _start(void) {
    int count = 0;
    while (1) {
        if (count >= 5) {
            sys_exit(0);
        }

        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);

        printf("[PONG] %d\n", count);

        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {~msg.payload[0], msg.payload[1] + 1};
        sys_ipc_reply(msg.reply_cap, 0, 0, 16, reply);
        
        count++;
    }
}
