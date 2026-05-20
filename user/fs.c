#include "lib.h"
#include "fs_proto.h"
#include "ns_proto.h"

#define FS_NAME_MAX 56

static void pack_name(uint64_t out[3], const char *name) {
    char *dst = (char *)out;

    out[0] = 0;
    out[1] = 0;
    out[2] = 0;

    for (int i = 0; i < 23 && name[i]; i++) {
        dst[i] = name[i];
    }
}

static void unpack_name(char *name, const uint64_t in[3]) {
    const char *src = (const char *)in;
    int i = 0;
    for (; i < 23 && src[i]; i++) {
        name[i] = src[i];
    }
    name[i] = '\0';
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void handle_list(uint32_t reply_cap, uint32_t index) {
    char name[FS_NAME_MAX];
    int file_cap = FS_BOOT_FILE_CAP_BASE + (int)index;
    int64_t size = sys_file_stat((uint32_t)file_cap, name, sizeof(name));
    if (size < 0) {
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {FS_RESP_END};
        sys_ipc_reply(reply_cap, 0, 0, 8, reply);
        return;
    }

    uint64_t packed[3];
    pack_name(packed, name);
    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)size, packed[0], packed[1], packed[2]};
    sys_ipc_reply(reply_cap, 0, 0, 32, reply);
}

static void handle_open_exec(uint32_t reply_cap, const uint64_t packed_name[3]) {
    char requested[24];
    unpack_name(requested, packed_name);

    for (uint32_t index = 0; index < 128; index++) {
        char name[FS_NAME_MAX];
        int64_t size = sys_file_stat(FS_BOOT_FILE_CAP_BASE + index, name, sizeof(name));
        if (size < 0) {
            break;
        }
        if (size >= 0 && streq(requested, name)) {
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
                (uint64_t)(FS_BOOT_EXEC_CAP_BASE + index)
            };
            sys_ipc_reply(reply_cap, 0, IPC_FLAG_CAP, 8, reply);
            return;
        }
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
    sys_ipc_reply(reply_cap, -1, 0, 8, reply);
}

static void handle_open_file(uint32_t reply_cap, const uint64_t packed_name[3]) {
    char requested[24];
    unpack_name(requested, packed_name);

    for (uint32_t index = 0; index < 128; index++) {
        char name[FS_NAME_MAX];
        int64_t size = sys_file_stat(FS_BOOT_FILE_CAP_BASE + index, name, sizeof(name));
        if (size < 0) {
            break;
        }
        if (streq(requested, name)) {
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
                (uint64_t)(FS_BOOT_FILE_CAP_BASE + index)
            };
            sys_ipc_reply(reply_cap, 0, IPC_FLAG_CAP, 8, reply);
            return;
        }
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
    sys_ipc_reply(reply_cap, -1, 0, 8, reply);
}

void _start(void) {
    while (ns_register("fs") < 0) {
        sys_sleep(10);
    }

    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);

        uint64_t request = msg.payload[IPC_WORD_OP];
        if (request == FS_REQ_LIST) {
            handle_list(msg.reply_cap, (uint32_t)msg.payload[IPC_WORD_ARG0]);
        } else if (request == FS_REQ_OPEN_EXEC) {
            handle_open_exec(msg.reply_cap, &msg.payload[IPC_WORD_ARG0]);
        } else if (request == FS_REQ_OPEN_FILE) {
            handle_open_file(msg.reply_cap, &msg.payload[IPC_WORD_ARG0]);
        } else {
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {FS_RESP_END};
            sys_ipc_reply(msg.reply_cap, -1, 0, 8, reply);
        }
    }
}
