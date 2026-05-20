#include "lib.h"
#include "ns_proto.h"

#define MAX_SERVICES 16

typedef struct {
    uint64_t name;
    uint32_t cap;
    int active;
} service_entry_t;

static service_entry_t services[MAX_SERVICES];

static int register_service(uint64_t name, uint32_t cap) {
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].active && services[i].name == name) {
            services[i].cap = cap;
            return 0;
        }
    }

    for (int i = 0; i < MAX_SERVICES; i++) {
        if (!services[i].active) {
            services[i].active = 1;
            services[i].name = name;
            services[i].cap = cap;
            return 0;
        }
    }

    return -1;
}

static uint32_t find_service(uint64_t name) {
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (services[i].active && services[i].name == name) {
            return services[i].cap;
        }
    }
    return 0;
}

void _start(void) {
    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        uint64_t request = msg.payload[IPC_WORD_OP];
        uint64_t name = msg.payload[IPC_WORD_ARG0];

        if (request == NS_REQ_REGISTER || msg.payload[IPC_CAP_WORD_OP] == NS_REQ_REGISTER) {
            uint32_t service_cap = (uint32_t)msg.payload[IPC_CAP_WORD_CAP];
            request = msg.payload[IPC_CAP_WORD_OP];
            name = msg.payload[IPC_CAP_WORD_ARG0];
            int status = request == NS_REQ_REGISTER ? register_service(name, service_cap) : -1;
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)status};
            sys_ipc_reply(msg.reply_cap, status, 0, 8, reply);
        } else if (request == NS_REQ_RESOLVE) {
            uint32_t service_cap = find_service(name);
            if (service_cap != 0) {
                uint64_t reply[IPC_REPLY_INLINE_WORDS] = {service_cap};
                sys_ipc_reply(msg.reply_cap, 0, IPC_FLAG_CAP, 8, reply);
            } else {
                uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
                sys_ipc_reply(msg.reply_cap, -1, 0, 8, reply);
            }
        } else {
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
            sys_ipc_reply(msg.reply_cap, -1, 0, 8, reply);
        }
    }
}
