#include "lib.h"

static uint64_t expected_word(uint32_t round, uint32_t word) {
    return 0xF000000000000000ULL | ((uint64_t)round << 32) | word;
}

void _start(void) {
    uint64_t lengths[4] = {0, 8, 64, 128};
    int all_ok = 1;

    for (uint32_t round = 0; round < 4; round++) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        uint64_t len = lengths[round];
        uint32_t words = (uint32_t)((len + 7) / 8);
        int ok = msg.len == len;

        for (uint32_t i = 0; ok && i < words; i++) {
            if (msg.payload[i] != expected_word(round, i)) {
                ok = 0;
            }
        }

        if (!ok) {
            all_ok = 0;
        }

        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
            ok ? 1ULL : 0ULL,
            len,
            words ? msg.payload[words - 1] : 0
        };
        sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 24, reply);
    }

    sys_exit(all_ok ? 0 : 1);
}
