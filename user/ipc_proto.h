#ifndef _IPC_PROTO_H_
#define _IPC_PROTO_H_

#define IPC_WORD_OP      0
#define IPC_WORD_ARG0    1
#define IPC_WORD_ARG1    2
#define IPC_WORD_ARG2    3

#define IPC_CAP_WORD_CAP 0
#define IPC_CAP_WORD_OP  1
#define IPC_CAP_WORD_ARG0 2

#define IPC_MEM_WORD_CAP        0
#define IPC_MEM_WORD_OFFSET     1
#define IPC_MEM_WORD_SIZE       2
#define IPC_MEM_WORD_DESCRIPTOR 3

#define IPC_STATUS_OK     0
#define IPC_STATUS_FAILED -1

#endif
