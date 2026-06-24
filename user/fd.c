#include "lib.h"

#define FD_MAX 16
#define FD_FIRST_FILE 3
#define FD_NAME_MAX 56
#define FD_PAGE_SIZE 4096ULL
#define FD_KIND_FILE 1
#define FD_KIND_PIPE_READ 2
#define FD_KIND_PIPE_WRITE 3
#define PIPE_WOULD_BLOCK (-2L)

typedef struct {
    int used;
    int kind;
    uint32_t pipe_cap;
    uint32_t handle;
    uint64_t size;
    uint64_t offset;
    int flags;
    char name[FD_NAME_MAX];
    uint8_t *page;
    uint32_t cached_page_index;
    int64_t cached_page_size;
} fd_entry_t;

static fd_entry_t fd_table[FD_MAX];
static uint8_t stdio_kind_checked;
static uint8_t stdio_is_pipe[3];

static int standard_is_pipe(int fd) {
    uint8_t bit = (uint8_t)(1U << fd);
    if ((stdio_kind_checked & bit) == 0) {
        cap_info_t cap = sys_capstat((uint32_t)(CAP_STDIN + fd));
        stdio_is_pipe[fd] = cap.present && cap.type == OCAP_PIPE;
        stdio_kind_checked |= bit;
    }
    return stdio_is_pipe[fd];
}

static int fd_can_read(const fd_entry_t *entry) {
    if (entry->kind == FD_KIND_PIPE_READ) return 1;
    if (entry->kind == FD_KIND_PIPE_WRITE) return 0;
    return (entry->flags & 3) != O_WRONLY;
}

static int fd_can_write(const fd_entry_t *entry) {
    if (entry->kind == FD_KIND_PIPE_WRITE) return 1;
    if (entry->kind == FD_KIND_PIPE_READ) return 0;
    return (entry->flags & 3) != O_RDONLY;
}

static int fd_allocate(void) {
    for (int fd = FD_FIRST_FILE; fd < FD_MAX; fd++) {
        if (!fd_table[fd].used) {
            fd_table[fd].used = 1;
            return fd;
        }
    }
    return -1;
}

static int fd_truncate(fd_entry_t *entry) {
    fs_write_page_t *request = (fs_write_page_t *)sys_mmap(FD_PAGE_SIZE);
    if (!request) {
        return -1;
    }
    memset(request, 0, FD_PAGE_SIZE);
    strncpy(request->name, entry->name, sizeof(request->name) - 1);
    request->flags = VFS_WRITE_FLAG_TRUNCATE;
    int64_t result = vfs_write_page(request);
    sys_munmap(request, FD_PAGE_SIZE);
    if (result < 0) {
        return -1;
    }
    entry->size = 0;
    entry->offset = 0;
    return 0;
}

int open(const char *path, int flags, ...) {
    if (!path || !path[0] || strlen(path) >= FD_NAME_MAX) {
        return -1;
    }
    int mode = flags & 3;
    if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR) {
        return -1;
    }

    int fd = fd_allocate();
    if (fd < 0) {
        return -1;
    }
    fd_entry_t *entry = &fd_table[fd];
    entry->kind = FD_KIND_FILE;
    entry->pipe_cap = 0;
    entry->handle = 0;
    entry->size = 0;
    entry->offset = 0;
    entry->flags = flags;
    entry->page = 0;
    entry->cached_page_index = 0;
    entry->cached_page_size = -1;
    strcpy(entry->name, path);

    vfs_file_info_t file = vfs_open_file(path);
    if (file.status == 0) {
        entry->handle = file.handle;
        entry->size = file.size;
    } else if ((flags & O_CREAT) == 0) {
        entry->used = 0;
        return -1;
    }

    if (flags & O_TRUNC) {
        if (!fd_can_write(entry) || fd_truncate(entry) < 0) {
            if (entry->handle) {
                vfs_close_handle(entry->handle);
            }
            entry->used = 0;
            return -1;
        }
    }
    return fd;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!buf || fd < 0 || fd >= FD_MAX || count == 0) {
        return count == 0 ? 0 : -1;
    }
    if (fd == 0 && standard_is_pipe(fd)) {
        for (;;) {
            long result = sys_pipe_read(CAP_STDIN, buf, count);
            if (result != PIPE_WOULD_BLOCK) return (ssize_t)result;
            sys_yield();
        }
    }
    if (fd_table[fd].used && fd_table[fd].kind == FD_KIND_PIPE_READ) {
        for (;;) {
            long result = sys_pipe_read(fd_table[fd].pipe_cap, buf, count);
            if (result != PIPE_WOULD_BLOCK) return (ssize_t)result;
            sys_yield();
        }
    }
    if (fd == 0) {
        ((char *)buf)[0] = terminal_read_char();
        return 1;
    }
    if (fd < FD_FIRST_FILE || !fd_table[fd].used ||
        !fd_can_read(&fd_table[fd]) || fd_table[fd].handle == 0) {
        return -1;
    }

    fd_entry_t *entry = &fd_table[fd];
    if (entry->offset >= entry->size) {
        return 0;
    }
    if (!entry->page) {
        entry->page = (uint8_t *)sys_mmap(FD_PAGE_SIZE);
        if (!entry->page) {
            return -1;
        }
    }

    size_t total = 0;
    while (total < count && entry->offset < entry->size) {
        uint32_t page_index = (uint32_t)(entry->offset / FD_PAGE_SIZE);
        uint32_t page_offset = (uint32_t)(entry->offset % FD_PAGE_SIZE);
        int64_t got;
        if (entry->cached_page_size >= 0 &&
            entry->cached_page_index == page_index) {
            got = entry->cached_page_size;
        } else {
            got = vfs_read_handle_page(entry->handle, page_index, entry->page);
            if (got >= 0) {
                entry->cached_page_index = page_index;
                entry->cached_page_size = got;
            }
        }
        if (got < 0) {
            return total ? (ssize_t)total : -1;
        }
        if ((uint64_t)got <= page_offset) {
            break;
        }
        size_t available = (size_t)((uint64_t)got - page_offset);
        size_t remaining = count - total;
        size_t file_remaining = (size_t)(entry->size - entry->offset);
        size_t chunk = available < remaining ? available : remaining;
        if (chunk > file_remaining) {
            chunk = file_remaining;
        }
        memcpy((uint8_t *)buf + total, entry->page + page_offset, chunk);
        total += chunk;
        entry->offset += chunk;
    }
    return (ssize_t)total;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!buf || fd < 0 || fd >= FD_MAX) {
        return -1;
    }
    if ((fd == 1 || fd == 2) && standard_is_pipe(fd)) {
        uint32_t cap = fd == 1 ? CAP_STDOUT : CAP_STDERR;
        size_t total = 0;
        while (total < count) {
            long result = sys_pipe_write(cap, (const uint8_t *)buf + total,
                                         count - total);
            if (result == PIPE_WOULD_BLOCK) { sys_yield(); continue; }
            if (result < 0) return total ? (ssize_t)total : -1;
            total += (size_t)result;
        }
        return (ssize_t)total;
    }
    if (fd_table[fd].used && fd_table[fd].kind == FD_KIND_PIPE_WRITE) {
        size_t total = 0;
        while (total < count) {
            long result = sys_pipe_write(fd_table[fd].pipe_cap,
                                         (const uint8_t *)buf + total,
                                         count - total);
            if (result == PIPE_WOULD_BLOCK) {
                sys_yield();
                continue;
            }
            if (result < 0) return total ? (ssize_t)total : -1;
            total += (size_t)result;
        }
        return (ssize_t)total;
    }
    if (fd == 1 || fd == 2) {
        return terminal_write(buf, count) < 0 ? -1 : (ssize_t)count;
    }
    if (fd < FD_FIRST_FILE || !fd_table[fd].used ||
        !fd_can_write(&fd_table[fd])) {
        return -1;
    }

    fd_entry_t *entry = &fd_table[fd];
    fs_write_page_t *request = (fs_write_page_t *)sys_mmap(FD_PAGE_SIZE);
    if (!request) {
        return -1;
    }

    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > FS_WRITE_DATA_MAX) {
            chunk = FS_WRITE_DATA_MAX;
        }
        memset(request, 0, FD_PAGE_SIZE);
        strncpy(request->name, entry->name, sizeof(request->name) - 1);
        request->offset = entry->offset;
        request->size = chunk;
        memcpy(request->data, (const uint8_t *)buf + total, chunk);
        int64_t wrote = vfs_write_page(request);
        if (wrote != (int64_t)chunk) {
            sys_munmap(request, FD_PAGE_SIZE);
            return total ? (ssize_t)total : -1;
        }
        total += chunk;
        entry->offset += chunk;
        if (entry->offset > entry->size) {
            entry->size = entry->offset;
        }
        entry->cached_page_size = -1;
    }
    sys_munmap(request, FD_PAGE_SIZE);
    return (ssize_t)total;
}

int close(int fd) {
    if (fd >= 0 && fd < FD_FIRST_FILE && !fd_table[fd].used) {
        return 0;
    }
    if (fd < FD_FIRST_FILE || fd >= FD_MAX || !fd_table[fd].used) {
        return -1;
    }
    fd_entry_t *entry = &fd_table[fd];
    if (entry->kind == FD_KIND_PIPE_READ || entry->kind == FD_KIND_PIPE_WRITE) {
        int result = sys_pipe_close(entry->pipe_cap);
        memset(entry, 0, sizeof(*entry));
        return result;
    }
    if (entry->handle) {
        vfs_close_handle(entry->handle);
    }
    if (entry->page) {
        sys_munmap(entry->page, FD_PAGE_SIZE);
    }
    entry->used = 0;
    entry->handle = 0;
    entry->page = 0;
    entry->cached_page_size = -1;
    return 0;
}

int pipe(int fds[2]) {
    if (!fds) return -1;
    int read_fd = fd_allocate();
    if (read_fd < 0) return -1;
    int write_fd = fd_allocate();
    if (write_fd < 0) {
        fd_table[read_fd].used = 0;
        return -1;
    }
    pipe_cap_pair_t caps = sys_pipe_create();
    if (caps.read_cap < 0 || caps.write_cap < 0) {
        fd_table[read_fd].used = 0;
        fd_table[write_fd].used = 0;
        return -1;
    }
    memset(&fd_table[read_fd], 0, sizeof(fd_table[read_fd]));
    fd_table[read_fd].used = 1;
    fd_table[read_fd].kind = FD_KIND_PIPE_READ;
    fd_table[read_fd].pipe_cap = (uint32_t)caps.read_cap;
    memset(&fd_table[write_fd], 0, sizeof(fd_table[write_fd]));
    fd_table[write_fd].used = 1;
    fd_table[write_fd].kind = FD_KIND_PIPE_WRITE;
    fd_table[write_fd].pipe_cap = (uint32_t)caps.write_cap;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int dup(int oldfd) {
    if (oldfd < 0 || oldfd >= FD_MAX || !fd_table[oldfd].used ||
        (fd_table[oldfd].kind != FD_KIND_PIPE_READ &&
         fd_table[oldfd].kind != FD_KIND_PIPE_WRITE)) return -1;
    int newfd = fd_allocate();
    if (newfd < 0) return -1;
    int cap = sys_pipe_dup(fd_table[oldfd].pipe_cap);
    if (cap < 0) {
        fd_table[newfd].used = 0;
        return -1;
    }
    fd_table[newfd] = fd_table[oldfd];
    fd_table[newfd].pipe_cap = (uint32_t)cap;
    return newfd;
}

int dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX ||
        !fd_table[oldfd].used) return -1;
    if (oldfd == newfd) return newfd;
    if (fd_table[oldfd].kind != FD_KIND_PIPE_READ &&
        fd_table[oldfd].kind != FD_KIND_PIPE_WRITE) return -1;
    int cap = sys_pipe_dup(fd_table[oldfd].pipe_cap);
    if (cap < 0) return -1;
    if (fd_table[newfd].used && close(newfd) < 0) {
        sys_pipe_close((uint32_t)cap);
        return -1;
    }
    fd_table[newfd] = fd_table[oldfd];
    fd_table[newfd].pipe_cap = (uint32_t)cap;
    return newfd;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < FD_FIRST_FILE || fd >= FD_MAX || !fd_table[fd].used ||
        fd_table[fd].kind != FD_KIND_FILE) {
        return -1;
    }
    fd_entry_t *entry = &fd_table[fd];
    off_t base = 0;
    if (whence == SEEK_CUR) {
        base = (off_t)entry->offset;
    } else if (whence == SEEK_END) {
        base = (off_t)entry->size;
    } else if (whence != SEEK_SET) {
        return -1;
    }
    if (offset < 0 && -offset > base) {
        return -1;
    }
    off_t next = base + offset;
    if (next < 0) {
        return -1;
    }
    entry->offset = (uint64_t)next;
    return next;
}
