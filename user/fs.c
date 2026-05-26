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
#define FS_EXEC_MAX_SIZE (1024ULL * 1024ULL)
typedef struct {
    char name[FS_NAME_MAX];
    uint64_t size;
} fs_entry_t;

typedef struct {
    uint32_t used;
    uint32_t entry_index;
    uint64_t offset;
} fs_handle_t;

static fs_entry_t *entries;
static fs_handle_t *handles;
static uint32_t entry_count;
static int table_loaded;
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



static int mount_fat_volume(void) {
    if (fat_mounted) {
        return 0;
    }

    if (fat_disk_init() < 0) {
        return -1;
    }

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

static int build_fs_table(void) {
    if (table_loaded) {
        return 0;
    }

    if (!entries) {
        entries = (fs_entry_t *)malloc(sizeof(fs_entry_t) * FS_MAX_FILES);
    }
    if (!handles) {
        handles = (fs_handle_t *)malloc(sizeof(fs_handle_t) * FS_MAX_HANDLES);
    }
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
        entry_count++;
    }

    f_closedir(&dir);
    table_loaded = 1;
    return 0;
}

static void update_table_entry(const char *name, uint64_t size) {
    if (!name) {
        return;
    }
    if (build_fs_table() < 0) {
        return;
    }

    char lowered[FS_NAME_MAX];
    copy_lower_string(lowered, name, sizeof(lowered));
    for (uint32_t i = 0; i < entry_count; i++) {
        if (streq(entries[i].name, lowered)) {
            entries[i].size = size;
            return;
        }
    }

    if (entry_count < FS_MAX_FILES) {
        copy_string(entries[entry_count].name, lowered, sizeof(entries[entry_count].name));
        entries[entry_count].size = size;
        entry_count++;
    }
}

static void remove_table_entry(const char *name) {
    if (!name || build_fs_table() < 0) {
        return;
    }

    char lowered[FS_NAME_MAX];
    copy_lower_string(lowered, name, sizeof(lowered));
    for (uint32_t i = 0; i < entry_count; i++) {
        if (!streq(entries[i].name, lowered)) {
            continue;
        }

        for (uint32_t h = 0; h < FS_MAX_HANDLES; h++) {
            if (!handles[h].used) {
                continue;
            }
            if (handles[h].entry_index == i) {
                handles[h].used = 0;
                handles[h].offset = 0;
            } else if (handles[h].entry_index > i) {
                handles[h].entry_index--;
            }
        }

        for (uint32_t j = i + 1; j < entry_count; j++) {
            entries[j - 1] = entries[j];
        }
        entry_count--;
        if (entry_count < FS_MAX_FILES) {
            entries[entry_count].name[0] = '\0';
            entries[entry_count].size = 0;
        }
        return;
    }
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
    uint32_t file_index = (uint32_t)(request.arg1 >> 32);
    uint64_t offset = (uint64_t)(request.arg1 & 0xFFFFFFFF) * PAGE_BYTES;

    if (build_fs_table() < 0 || file_index >= entry_count || request.arg2 == 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[file_index];
    char *remote = (char *)sys_vfs_inject(request.client_tid,
                                          (void *)request.arg2, 1);
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

static void handle_vfs_write_page(vfs_request_t request) {
    if (request.arg1 == 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_write_page_t *remote = (fs_write_page_t *)sys_vfs_inject(request.client_tid,
                                                               (void *)request.arg1, 1);
    if (!remote) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    int ok = 0;
    uint64_t written64 = 0;
    uint64_t file_size = 0;

    if (mount_fat_volume() == 0 &&
        remote->name[0] != '\0' &&
        remote->size <= FS_WRITE_DATA_MAX) {
        BYTE mode = FA_WRITE | FA_OPEN_ALWAYS;
        if (remote->flags & VFS_WRITE_FLAG_TRUNCATE) {
            mode = FA_WRITE | FA_CREATE_ALWAYS;
        }

        FIL file;
        if (f_open(&file, remote->name, mode) == FR_OK) {
            if (f_lseek(&file, remote->offset) == FR_OK) {
                UINT wrote = 0;
                if (f_write(&file, remote->data, (UINT)remote->size, &wrote) == FR_OK &&
                    wrote == (UINT)remote->size &&
                    f_sync(&file) == FR_OK) {
                    written64 = wrote;
                    file_size = (uint64_t)f_size(&file);
                    update_table_entry(remote->name, file_size);
                    ok = 1;
                }
            }
            f_close(&file);
        }
    }

    sys_munmap(remote, PAGE_BYTES);
    sys_vfs_reply(ok ? 0 : -1, written64, file_size, 0);
}

static void handle_vfs_delete(vfs_request_t request) {
    if (request.arg1 == 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    char *remote = (char *)sys_vfs_inject(request.client_tid,
                                          (void *)request.arg1, 1);
    if (!remote) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    char name[FS_NAME_MAX];
    copy_string(name, remote, sizeof(name));
    sys_munmap(remote, PAGE_BYTES);

    int ok = 0;
    if (mount_fat_volume() == 0 && name[0] != '\0' &&
        f_unlink(name) == FR_OK) {
        remove_table_entry(name);
        ok = 1;
    }

    sys_vfs_reply(ok ? 0 : -1, 0, 0, 0);
}

static void handle_vfs_exec_index(vfs_request_t request) {
    if (build_fs_table() < 0 || request.arg1 >= entry_count) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    fs_entry_t *entry = &entries[(uint32_t)request.arg1];
    if (entry->size == 0 || entry->size > FS_EXEC_MAX_SIZE) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    uint64_t aligned_size = (entry->size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
    uint8_t *elf = (uint8_t *)malloc((size_t)aligned_size);
    if (!elf) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    uint64_t loaded = 0;
    for (uint64_t offset = 0; offset < aligned_size; offset += PAGE_BYTES) {
        uint64_t got = 0;
        uint64_t file_size = 0;
        if (read_fat_file_page(entry->name, offset, elf + offset, PAGE_BYTES,
                               &got, &file_size) < 0) {
            free(elf);
            sys_vfs_reply(-1, 0, 0, 0);
            return;
        }
        loaded += got;
        if (got < PAGE_BYTES) {
            break;
        }
    }

    if (loaded < entry->size ||
        elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
        free(elf);
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    int exec_cap = sys_vfs_exec_create(request.client_tid, elf, entry->size,
                                       (uint32_t)request.arg1);
    free(elf);
    if (exec_cap < 0) {
        sys_vfs_reply(-1, 0, 0, 0);
        return;
    }

    sys_vfs_reply(0, (uint32_t)exec_cap, entry->size, request.arg1);
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
        } else if (request.arg0 == VFS_FS_REQ_WRITE_PAGE) {
            handle_vfs_write_page(request);
        } else if (request.arg0 == VFS_FS_REQ_DELETE) {
            handle_vfs_delete(request);
        } else {
            sys_vfs_reply(-1, 0, 0, 0);
        }
    }
}

void _start(void) {
    (void)build_fs_table();
    run_vfs_server();
}
