#ifndef _FS_PROTO_H_
#define _FS_PROTO_H_

#include "ipc_proto.h"

#define FS_REQ_LIST 1
#define FS_REQ_OPEN_EXEC 2
#define FS_REQ_OPEN_FILE 3
#define FS_RESP_END 0xFFFFFFFFFFFFFFFFUL

#define VFS_ID_FS 1
#define VFS_FS_REQ_PING 1
#define VFS_FS_REQ_LIST 2
#define VFS_FS_REQ_READ_INDEX 3
#define VFS_FS_REQ_EXEC_INDEX 4
#define VFS_FS_REQ_OPEN_INDEX 5
#define VFS_FS_REQ_READ_HANDLE_PAGE 6
#define VFS_FS_REQ_CLOSE_HANDLE 7

#define VFS_HANDLE_SHIFT 32

#define USER_BOOT_INITRD_BASE 0xD0000000ULL
#define USER_BOOT_INITRD_MAX_SIZE (8 * 1024 * 1024)

#define INITRD_MAGIC 0x4452494E
#define INITRD_VERSION 1
#define INITRD_NAME_LEN 56

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t header_size;
} initrd_header_t;

typedef struct {
    char name[INITRD_NAME_LEN];
    uint64_t offset;
    uint64_t size;
} initrd_entry_t;

// Boot grants the FS server one local exec cap per initrd index starting here.
#define FS_BOOT_EXEC_CAP_BASE 64
#define FS_BOOT_FILE_CAP_BASE 128

#endif
