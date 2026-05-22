#include "lib.h"
#include "malloc.h"
#include "fs_proto.h"
#include "ns_proto.h"
#include "fat_diskio.h"
#include "ff.h"

#define FS_NAME_MAX 56
#define FS_MAX_FILES 128
#define FS_MAX_HANDLES 128
#define PAGE_BYTES 4096ULL

typedef struct {
    char name[FS_NAME_MAX];
    uint64_t size;
    int file_cap;
    int exec_cap;
} fs_entry_t;

typedef struct {
    uint32_t used;
    uint32_t entry_index;
    uint64_t offset;
} fs_handle_t;

static fs_entry_t *entries;
static fs_handle_t *handles;
static uint32_t entry_count;
static FATFS fat_fs;
static int fat_mounted;

static void copy_string(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (cap == 0) {
        return;
    }
    for (; i + 1 < cap && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static char lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static void copy_lower_string(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (cap == 0) {
        return;
    }
    for (; i + 1 < cap && src[i]; i++) {
        dst[i] = lower_ascii(src[i]);
    }
    dst[i] = '\0';
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const initrd_header_t *outer_header(void) {
    const unsigned char *image = (const unsigned char *)USER_BOOT_INITRD_BASE;
    const initrd_header_t *header = (const initrd_header_t *)image;
    if (header->magic != INITRD_MAGIC ||
        header->version != INITRD_VERSION ||
        header->file_count == 0 ||
        header->file_count > FS_MAX_FILES ||
        header->header_size < sizeof(initrd_header_t) +
                              ((uint64_t)header->file_count * sizeof(initrd_entry_t)) ||
        header->header_size > USER_BOOT_INITRD_MAX_SIZE) {
        return 0;
    }
    return header;
}

static const initrd_entry_t *outer_entries(const initrd_header_t *header) {
    if (!header) {
        return 0;
    }
    return (const initrd_entry_t *)((const unsigned char *)USER_BOOT_INITRD_BASE +
                                   sizeof(initrd_header_t));
}

static int outer_find_file(const char *name, const unsigned char **data,
                           uint64_t *size, uint32_t *index_out) {
    const initrd_header_t *header = outer_header();
    const initrd_entry_t *rd_entries = outer_entries(header);
    if (!header || !rd_entries || !name) {
        return -1;
    }

    for (uint32_t index = 0; index < header->file_count; index++) {
        const initrd_entry_t *rd = &rd_entries[index];
        if (rd->offset > USER_BOOT_INITRD_MAX_SIZE ||
            rd->size > USER_BOOT_INITRD_MAX_SIZE - rd->offset ||
            rd->name[0] == '\0') {
            return -1;
        }
        if (streq(name, rd->name)) {
            if (data) {
                *data = (const unsigned char *)USER_BOOT_INITRD_BASE + rd->offset;
            }
            if (size) {
                *size = rd->size;
            }
            if (index_out) {
                *index_out = index;
            }
            return 0;
        }
    }

    return -1;
}

static uint32_t outer_index_for_name(const char *name) {
    uint32_t index = (uint32_t)-1;
    if (outer_find_file(name, 0, 0, &index) < 0) {
        return (uint32_t)-1;
    }
    return index;
}

static int mount_fat_volume(void) {
    if (fat_mounted) {
        return 0;
    }

    const unsigned char *fat_data = 0;
    uint64_t fat_size = 0;
    if (outer_find_file("apps.fat", &fat_data, &fat_size, 0) < 0 ||
        !fat_data || fat_size < 512) {
        return -1;
    }

    fat_disk_set_image(fat_data, fat_size);
    if (f_mount(&fat_fs, "", 1) != FR_OK) {
        return -1;
    }

    fat_mounted = 1;
    return 0;
}

static int read_fat_file_page(const char *name, uint64_t offset,
                              void *dst, uint64_t dst_cap,
                              uint64_t *bytes_read, uint64_t *file_size) {
    if (mount_fat_volume() < 0 || !name || !dst || dst_cap == 0) {
        return -1;
    }

    FIL file;
    if (f_open(&file, name, FA_READ) != FR_OK) {
        return -1;
    }

    uint64_t size = (uint64_t)f_size(&file);
    if (file_size) {
        *file_size = size;
    }
    if (offset >= size) {
        if (bytes_read) {
            *bytes_read = 0;
        }
        f_close(&file);
        return 0;
    }

    uint64_t want = size - offset;
    if (want > dst_cap) {
        want = dst_cap;
    }
    if (f_lseek(&file, offset) != FR_OK) {
        f_close(&file);
        return -1;
    }

    UINT got = 0;
    if (f_read(&file, dst, (UINT)want, &got) != FR_OK) {
        f_close(&file);
        return -1;
    }

    f_close(&file);
    if (bytes_read) {
        *bytes_read = got;
    }
    return 0;
}

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

static int build_fs_table(void) {
    if (entries) {
        return 0;
    }

    entries = (fs_entry_t *)malloc(sizeof(fs_entry_t) * FS_MAX_FILES);
    handles = (fs_handle_t *)malloc(sizeof(fs_handle_t) * FS_MAX_HANDLES);
    if (!entries || !handles) {
        return -1;
    }

    for (uint32_t i = 0; i < FS_MAX_HANDLES; i++) {
        handles[i].used = 0;
        handles[i].entry_index = 0;
        handles[i].offset = 0;
    }

    entry_count = 0;
    if (mount_fat_volume() < 0) {
        return -1;
    }

    DIR dir;
    if (f_opendir(&dir, "/") != FR_OK) {
        return -1;
    }

    while (entry_count < FS_MAX_FILES) {
        FILINFO info;
        if (f_readdir(&dir, &info) != FR_OK) {
            return -1;
        }
        if (info.fname[0] == '\0') {
            break;
        }
        if (info.fattrib & AM_DIR) {
            continue;
        }

        fs_entry_t *entry = &entries[entry_count];
        copy_lower_string(entry->name, info.fname, sizeof(entry->name));
        entry->size = (uint64_t)info.fsize;

        uint32_t raw_index = outer_index_for_name(entry->name);
        if (raw_index == (uint32_t)-1) {
            entry->file_cap = -1;
            entry->exec_cap = -1;
        } else {
            entry->file_cap = FS_BOOT_FILE_CAP_BASE + (int)raw_index;
            entry->exec_cap = FS_BOOT_EXEC_CAP_BASE + (int)raw_index;
        }
        entry_count++;
    }

    f_closedir(&dir);
    return entry_count == 0 ? -1 : 0;
}

static void handle_list(uint32_t reply_cap, uint32_t index) {
    if (build_fs_table() < 0 || index >= entry_count) {
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {FS_RESP_END};
        sys_ipc_reply(reply_cap, 0, 0, 8, reply);
        return;
    }

    uint64_t packed[3];
    pack_name(packed, entries[index].name);
    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
        entries[index].size, packed[0], packed[1], packed[2]
    };
    sys_ipc_reply(reply_cap, 0, 0, 32, reply);
}

static void handle_open_exec(uint32_t reply_cap, const uint64_t packed_name[3]) {
    char requested[24];
    unpack_name(requested, packed_name);

    if (build_fs_table() == 0) {
        for (uint32_t index = 0; index < entry_count; index++) {
            if (streq(requested, entries[index].name) && entries[index].exec_cap >= 0) {
                uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
                    (uint64_t)entries[index].exec_cap
                };
                sys_ipc_reply(reply_cap, 0, IPC_FLAG_CAP, 8, reply);
                return;
            }
        }
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
    sys_ipc_reply(reply_cap, -1, 0, 8, reply);
}

static void handle_open_file(uint32_t reply_cap, const uint64_t packed_name[3]) {
    char requested[24];
    unpack_name(requested, packed_name);

    if (build_fs_table() == 0) {
        for (uint32_t index = 0; index < entry_count; index++) {
            if (streq(requested, entries[index].name) && entries[index].file_cap >= 0) {
                uint64_t reply[IPC_REPLY_INLINE_WORDS] = {
                    (uint64_t)entries[index].file_cap
                };
                sys_ipc_reply(reply_cap, 0, IPC_FLAG_CAP, 8, reply);
                return;
            }
        }
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {(uint64_t)-1};
    sys_ipc_reply(reply_cap, -1, 0, 8, reply);
}

static void handle_vfs_list(vfs_request_t request) {
    if (build_fs_table() < 0 || request.arg1 >= entry_count || request.arg2 == 0) {
        sys_vfs_reply(0, FS_RESP_END, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[(uint32_t)request.arg1];
    char *remote = (char *)sys_vfs_inject(request.client_tid,
                                          (void *)request.arg2, 1);
    if (!remote) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    copy_string(remote, entry->name, FS_NAME_MAX);
    sys_munmap(remote, PAGE_BYTES);
    sys_vfs_reply(0, entry->size, request.arg2, (uint64_t)remote);
}

static void handle_vfs_read(vfs_request_t request) {
    if (build_fs_table() < 0 || request.arg1 >= entry_count || request.arg2 == 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[(uint32_t)request.arg1];
    char *remote = (char *)sys_vfs_inject(request.client_tid,
                                          (void *)request.arg2, 1);
    if (!remote) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    uint64_t copy_len = 0;
    uint64_t file_size = 0;
    if (read_fat_file_page(entry->name, 0, remote, PAGE_BYTES,
                           &copy_len, &file_size) < 0) {
        sys_munmap(remote, PAGE_BYTES);
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }
    sys_munmap(remote, PAGE_BYTES);
    sys_vfs_reply(0, copy_len, request.arg2, file_size);
}

static int alloc_handle(uint32_t entry_index) {
    if (!handles || entry_index >= entry_count) {
        return -1;
    }

    for (uint32_t i = 0; i < FS_MAX_HANDLES; i++) {
        if (!handles[i].used) {
            handles[i].used = 1;
            handles[i].entry_index = entry_index;
            handles[i].offset = 0;
            return (int)(i + 1);
        }
    }

    return -1;
}

static fs_entry_t *entry_from_handle(uint32_t handle) {
    if (!handles || handle == 0 || handle > FS_MAX_HANDLES) {
        return 0;
    }

    fs_handle_t *slot = &handles[handle - 1];
    if (!slot->used || slot->entry_index >= entry_count) {
        return 0;
    }

    return &entries[slot->entry_index];
}

static void handle_vfs_open_index(vfs_request_t request) {
    if (build_fs_table() < 0 || request.arg1 >= entry_count) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    int handle = alloc_handle((uint32_t)request.arg1);
    if (handle < 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[(uint32_t)request.arg1];
    sys_vfs_reply(0, (uint32_t)handle, entry->size, request.arg1);
}

static void handle_vfs_read_handle_page(vfs_request_t request) {
    uint32_t handle = (uint32_t)(request.arg1 >> VFS_HANDLE_SHIFT);
    uint32_t page_index = (uint32_t)request.arg1;
    uint64_t client_va = request.arg2;

    if (build_fs_table() < 0 || client_va == 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = entry_from_handle(handle);
    if (!entry) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    uint64_t offset = (uint64_t)page_index * PAGE_BYTES;
    if (offset >= entry->size) {
        sys_vfs_reply(0, 0, offset, entry->size);
        return;
    }

    char *remote = (char *)sys_vfs_inject(request.client_tid,
                                          (void *)client_va, 1);
    if (!remote) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    uint64_t copy_len = 0;
    uint64_t file_size = 0;
    if (read_fat_file_page(entry->name, offset, remote, PAGE_BYTES,
                           &copy_len, &file_size) < 0) {
        sys_munmap(remote, PAGE_BYTES);
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }
    sys_munmap(remote, PAGE_BYTES);

    if (handle <= FS_MAX_HANDLES) {
        handles[handle - 1].offset = offset + copy_len;
    }
    sys_vfs_reply(0, copy_len, offset, file_size);
}

static void handle_vfs_close_handle(vfs_request_t request) {
    uint32_t handle = (uint32_t)request.arg1;
    if (!handles || handle == 0 || handle > FS_MAX_HANDLES ||
        !handles[handle - 1].used) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    handles[handle - 1].used = 0;
    handles[handle - 1].entry_index = 0;
    handles[handle - 1].offset = 0;
    sys_vfs_reply(0, 0, 0, 0);
}

static void handle_vfs_exec_index(vfs_request_t request) {
    if (build_fs_table() < 0 || request.arg1 >= entry_count) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[(uint32_t)request.arg1];
    sys_vfs_reply(0, request.arg1, entry->size, 0);
}

static void run_vfs_server(void) {
    while (ns_register("vfs") < 0) {
        sys_sleep(10);
    }

    while (1) {
        vfs_request_t request = sys_vfs_recv();
        if (request.status < 0 || request.vfs_id != VFS_ID_FS) {
            continue;
        }

        if (request.arg0 == VFS_FS_REQ_PING) {
            sys_vfs_reply(0, request.arg1 + 1, request.arg2, request.client_tid);
        } else if (request.arg0 == VFS_FS_REQ_LIST) {
            handle_vfs_list(request);
        } else if (request.arg0 == VFS_FS_REQ_READ_INDEX) {
            handle_vfs_read(request);
        } else if (request.arg0 == VFS_FS_REQ_EXEC_INDEX) {
            handle_vfs_exec_index(request);
        } else if (request.arg0 == VFS_FS_REQ_OPEN_INDEX) {
            handle_vfs_open_index(request);
        } else if (request.arg0 == VFS_FS_REQ_READ_HANDLE_PAGE) {
            handle_vfs_read_handle_page(request);
        } else if (request.arg0 == VFS_FS_REQ_CLOSE_HANDLE) {
            handle_vfs_close_handle(request);
        } else {
            sys_vfs_reply(-1, 0, 0, 0);
        }
    }
}

static void run_ipc_server(void) {
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

void _start(void) {
    (void)build_fs_table();

    int forked = sys_fork();
    if (forked == 0) {
        run_vfs_server();
    }

    run_ipc_server();
}
