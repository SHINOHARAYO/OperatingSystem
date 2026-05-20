#include "lib.h"

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

void _start(void) {
    ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
    char *buf = (char *)msg.payload[0];
    int ok = buf && starts_with(buf, "share:from-shell");

    if (ok) {
        buf[0] = 'S';
        buf[1] = 'H';
        buf[32] = '!';
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {ok ? 1ULL : 0ULL, (uint64_t)buf, ok ? (uint64_t)buf[32] : 0};
    sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 24, reply);
    sys_exit(ok ? 0 : 1);
}
