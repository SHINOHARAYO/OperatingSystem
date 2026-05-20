#ifndef _FS_PROTO_H_
#define _FS_PROTO_H_

#include "ipc_proto.h"

#define FS_REQ_LIST 1
#define FS_REQ_OPEN_EXEC 2
#define FS_REQ_OPEN_FILE 3
#define FS_RESP_END 0xFFFFFFFFFFFFFFFFUL

// Boot grants the FS server one local exec cap per initrd index starting here.
#define FS_BOOT_EXEC_CAP_BASE 64
#define FS_BOOT_FILE_CAP_BASE 128

#endif
