#include "lib.h"
#include "ns_proto.h"
#include "fs_proto.h"
#include <stdarg.h>

static int uart_cap = -1;

static int get_uart_cap(void) {
    if (uart_cap < 0) {
        uart_cap = ns_resolve("uart");
    }
    return uart_cap;
}

static void _flush_buf(const char *buf, int len) {
    int offset = 0;
    while (offset < len) {
        uint64_t chunks[IPC_INLINE_WORDS] = {0};
        char *dst = (char *)chunks;
        int i;
        for (i = 0; i < (int)IPC_INLINE_BYTES && offset < len; i++, offset++) {
            dst[i] = buf[offset];
        }
        int cap = get_uart_cap();
        if (cap < 0) {
            return;
        }

        sys_ipc_call((uint32_t)cap, 0, (uint64_t)i, chunks);
    }
}

static inline void _put(char *buf, int *pos, char c) {
    buf[(*pos)++] = c;
}

static inline void _maybe_flush(char *buf, int *pos) {
    if (*pos >= 240) {
        _flush_buf(buf, *pos);
        *pos = 0;
    }
}

static void _fmt_uint(char *buf, int *pos, uint64_t val, unsigned int base) {
    const char *digits = "0123456789abcdef";
    if (val == 0) { _put(buf, pos, '0'); return; }
    char tmp[24]; int n = 0;
    while (val) { tmp[n++] = digits[val % base]; val /= base; }
    for (int i = n - 1; i >= 0; i--) _put(buf, pos, tmp[i]);
}


void *memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int value, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

int memcmp(const void *a, const void *b, uint64_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (uint64_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

uint64_t strlen(const char *s) {
    uint64_t len = 0;
    while (s && s[len]) {
        len++;
    }
    return len;
}

char *strchr(const char *s, int c) {
    if (!s) {
        return 0;
    }
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == 0 ? (char *)s : 0;
}

void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    if (len) _flush_buf(s, len);
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buf[256];
    int pos = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            _put(buf, &pos, *p);
            _maybe_flush(buf, &pos);
            continue;
        }
        p++;
        if (!*p) break;

        switch (*p) {
            case 'd': {
                int val = va_arg(args, int);
                if (val < 0) { _put(buf, &pos, '-'); val = -val; }
                _fmt_uint(buf, &pos, (uint64_t)(unsigned int)val, 10);
                break;
            }
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                _fmt_uint(buf, &pos, (uint64_t)val, 10);
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                _fmt_uint(buf, &pos, (uint64_t)val, 16);
                break;
            }
            case 'l': {
                p++;
                if (*p == 'u') {
                    uint64_t val = va_arg(args, uint64_t);
                    _fmt_uint(buf, &pos, val, 10);
                } else if (*p == 'x') {
                    uint64_t val = va_arg(args, uint64_t);
                    _fmt_uint(buf, &pos, val, 16);
                } else if (*p == 'd') {
                    int64_t val = va_arg(args, int64_t);
                    if (val < 0) { _put(buf, &pos, '-'); val = -val; }
                    _fmt_uint(buf, &pos, (uint64_t)val, 10);
                } else {
                    _put(buf, &pos, '%');
                    _put(buf, &pos, 'l');
                    _put(buf, &pos, *p);
                }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) { _put(buf, &pos, *s++); _maybe_flush(buf, &pos); }
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                _put(buf, &pos, c);
                break;
            }
            case '%': {
                _put(buf, &pos, '%');
                break;
            }
            default: {
                _put(buf, &pos, '%');
                _put(buf, &pos, *p);
                break;
            }
        }
        _maybe_flush(buf, &pos);
    }

    if (pos > 0) _flush_buf(buf, pos);
    va_end(args);
}

int streq(const char *a, const char *b) {
    while (*a && *b && (*a == *b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int vfs_bound = 0;
static int vfs_cap = -1;
#define PAGE_BYTES 4096ULL

int ensure_vfs_bound(void) {
    if (vfs_bound) {
        return 0;
    }
    if (vfs_cap < 0) {
        vfs_cap = ns_resolve("vfs");
    }
    if (vfs_cap < 0) {
        return -1;
    }
    if (sys_vfs_bind(VFS_ID_FS, (uint32_t)vfs_cap) < 0) {
        return -1;
    }
    vfs_bound = 1;
    return 0;
}

vfs_reply_t vfs_call_retry(uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    vfs_reply_t reply;
    reply.status = -1;
    reply.value0 = 0;
    reply.value1 = 0;
    reply.value2 = 0;

    for (uint32_t attempt = 0; attempt < 20; attempt++) {
        reply = sys_vfs_call(VFS_ID_FS, arg0, arg1, arg2);
        if (reply.status >= 0) {
            return reply;
        }
        sys_sleep(10);
    }
    return reply;
}

vfs_file_info_t vfs_open_file(const char *target) {
    vfs_file_info_t info;
    info.status = -1;
    info.index = 0;
    info.handle = 0;
    info.size = 0;

    if (ensure_vfs_bound() < 0) {
        return info;
    }

    for (uint32_t index = 0; index < 128; index++) {
        char *name = (char *)sys_mmap(PAGE_BYTES);
        if (!name) {
            return info;
        }

        vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_LIST, index, (uint64_t)name);
        if (reply.status < 0 || reply.value0 == 0xFFFFFFFFFFFFFFFFUL) {
            sys_munmap(name, PAGE_BYTES);
            return info;
        }
        if (streq(name, target)) {
            vfs_reply_t opened = vfs_call_retry(VFS_FS_REQ_OPEN_INDEX, index, 0);
            sys_munmap(name, PAGE_BYTES);
            if (opened.status < 0 || opened.value0 == 0) {
                return info;
            }
            info.status = 0;
            info.index = index;
            info.handle = (uint32_t)opened.value0;
            info.size = opened.value1;
            return info;
        }
        sys_munmap(name, PAGE_BYTES);
    }
    return info;
}

int64_t vfs_read_index(uint32_t index, void *buf) {
    if (!buf || ensure_vfs_bound() < 0) {
        return -1;
    }
    vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_READ_INDEX, index, (uint64_t)buf);
    return reply.status < 0 ? -1 : (int64_t)reply.value0;
}

int64_t vfs_read_handle_page(uint32_t handle, uint32_t page_index, void *buf) {
    if (!buf || handle == 0 || ensure_vfs_bound() < 0) {
        return -1;
    }
    uint64_t descriptor = ((uint64_t)handle << VFS_HANDLE_SHIFT) | page_index;
    vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_READ_HANDLE_PAGE, descriptor, (uint64_t)buf);
    return reply.status < 0 ? -1 : (int64_t)reply.value0;
}

int64_t vfs_write_page(fs_write_page_t *request) {
    if (!request || request->name[0] == '\0' ||
        request->size > FS_WRITE_DATA_MAX ||
        ensure_vfs_bound() < 0) {
        return -1;
    }

    vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_WRITE_PAGE, (uint64_t)request, 0);
    return reply.status < 0 ? -1 : (int64_t)reply.value0;
}

static void vfs_copy_name(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    for (; i + 1 < cap && src && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int vfs_delete_file(const char *name) {
    if (!name || name[0] == '\0' || ensure_vfs_bound() < 0) {
        return -1;
    }

    char *remote_name = (char *)sys_mmap(PAGE_BYTES);
    if (!remote_name) {
        return -1;
    }

    memset(remote_name, 0, PAGE_BYTES);
    vfs_copy_name(remote_name, name, PAGE_BYTES);
    vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_DELETE, (uint64_t)remote_name, 0);
    sys_munmap(remote_name, PAGE_BYTES);
    return reply.status < 0 ? -1 : 0;
}

int64_t vfs_copy_file(const char *src, const char *dst) {
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0' || streq(src, dst)) {
        return -1;
    }

    vfs_file_info_t info = vfs_open_file(src);
    if (info.status < 0 || info.size == 0) {
        return -1;
    }

    uint8_t *read_page = (uint8_t *)sys_mmap(PAGE_BYTES);
    fs_write_page_t *write = (fs_write_page_t *)sys_mmap(PAGE_BYTES);
    if (!read_page || !write) {
        if (read_page) sys_munmap(read_page, PAGE_BYTES);
        if (write) sys_munmap(write, PAGE_BYTES);
        vfs_close_handle(info.handle);
        return -1;
    }

    uint64_t copied = 0;
    uint32_t page_index = 0;
    while (copied < info.size) {
        int64_t got = vfs_read_handle_page(info.handle, page_index, read_page);
        if (got <= 0) {
            copied = (uint64_t)-1;
            break;
        }

        uint64_t page_offset = 0;
        while (page_offset < (uint64_t)got && copied < info.size) {
            uint64_t chunk = (uint64_t)got - page_offset;
            if (chunk > FS_WRITE_DATA_MAX) {
                chunk = FS_WRITE_DATA_MAX;
            }
            if (chunk > info.size - copied) {
                chunk = info.size - copied;
            }

            memset(write, 0, PAGE_BYTES);
            vfs_copy_name(write->name, dst, sizeof(write->name));
            write->offset = copied;
            write->size = chunk;
            write->flags = copied == 0 ? VFS_WRITE_FLAG_TRUNCATE : 0;
            memcpy(write->data, read_page + page_offset, chunk);

            int64_t wrote = vfs_write_page(write);
            if (wrote != (int64_t)chunk) {
                copied = (uint64_t)-1;
                break;
            }

            copied += chunk;
            page_offset += chunk;
        }

        if (copied == (uint64_t)-1) {
            break;
        }
        page_index++;
    }

    vfs_close_handle(info.handle);
    sys_munmap(read_page, PAGE_BYTES);
    sys_munmap(write, PAGE_BYTES);
    return copied == (uint64_t)-1 ? -1 : (int64_t)copied;
}

void vfs_close_handle(uint32_t handle) {
    if (handle == 0 || ensure_vfs_bound() < 0) {
        return;
    }
    (void)vfs_call_retry(VFS_FS_REQ_CLOSE_HANDLE, handle, 0);
}

spawn_result_t vfs_spawn_program(const char *name, uint8_t priority) {
    spawn_result_t fail = { .tid = (uint32_t)-1, .endpoint_cap = -1 };

    vfs_file_info_t info = vfs_open_file(name);
    if (info.status < 0 || info.size == 0) {
        return fail;
    }

    vfs_close_handle(info.handle);

    vfs_reply_t exec = vfs_call_retry(VFS_FS_REQ_EXEC_INDEX, info.index, 0);
    if (exec.status < 0 || exec.value0 == 0) {
        return fail;
    }

    return sys_spawn_exec2((uint32_t)exec.value0, priority);
}

uint64_t page_align_size(uint64_t size) {
    return (size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
}
