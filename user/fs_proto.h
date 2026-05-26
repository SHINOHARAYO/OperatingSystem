#ifndef _FS_PROTO_H_
#define _FS_PROTO_H_

#include "ipc_proto.h"


#define FS_RESP_END 0xFFFFFFFFFFFFFFFFUL

#define VFS_ID_FS 1
#define VFS_FS_REQ_PING 1
#define VFS_FS_REQ_LIST 2
#define VFS_FS_REQ_READ_INDEX 3
#define VFS_FS_REQ_EXEC_INDEX 4
#define VFS_FS_REQ_OPEN_INDEX 5
#define VFS_FS_REQ_READ_HANDLE_PAGE 6
#define VFS_FS_REQ_CLOSE_HANDLE 7
#define VFS_FS_REQ_WRITE_PAGE 8
#define VFS_FS_REQ_DELETE 9

#define VFS_HANDLE_SHIFT 32
#define VFS_WRITE_FLAG_TRUNCATE 1ULL

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

#define FS_WRITE_DATA_OFFSET 80
#define FS_WRITE_DATA_MAX (4096 - FS_WRITE_DATA_OFFSET)

typedef struct {
    char name[56];
    uint64_t offset;
    uint64_t size;
    uint64_t flags;
    unsigned char data[FS_WRITE_DATA_MAX];
} fs_write_page_t;

#endif
