#include "lib.h"
#include "ns_proto.h"

#define SERVICE_PRIORITY 5
#define SHELL_PRIORITY 10
#define PAGE_BYTES 4096ULL

static int eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (uint32_t i = 0; path && path[i]; i++) {
        if (path[i] == '/') {
            base = path + i + 1;
        }
    }
    return base;
}

static void service_name_from_path(const char *path, char *out, uint32_t cap) {
    const char *base = base_name(path);
    uint32_t i = 0;
    if (!out || cap == 0) {
        return;
    }
    for (; i + 1 < cap && base[i] && base[i] != '.'; i++) {
        out[i] = base[i];
    }
    out[i] = '\0';
    if (eq(out, "fs")) {
        out[0] = 'v';
        out[1] = 'f';
        out[2] = 's';
        out[3] = '\0';
    }
}

static int is_bootstrap_service(const char *path) {
    const char *base = base_name(path);
    return eq(base, "ns.elf") || eq(base, "block.elf") || eq(base, "fs.elf") ||
           eq(base, "init.elf");
}

static void spawn_initrd_service(const char *name) {
    for (uint32_t attempt = 0; attempt < 20; attempt++) {
        spawn_result_t result = sys_spawn_file2(name, SERVICE_PRIORITY);
        if ((int)result.tid >= 0) {
            return;
        }
        sys_sleep(20);
    }
}

static void wait_for_service(const char *name) {
    for (uint32_t attempt = 0; attempt < 200; attempt++) {
        if (ns_resolve(name) >= 0) {
            return;
        }
        sys_sleep(10);
    }
}

static void launch_manifest_entry(const char *kind, const char *path) {
    if (eq(kind, "service")) {
        char service[16];
        service_name_from_path(path, service, sizeof(service));
        if (service[0] && ns_resolve(service) >= 0) {
            return;
        }
        if (is_bootstrap_service(path)) {
            return;
        }
        (void)vfs_spawn_program(path, SERVICE_PRIORITY);
        if (service[0]) {
            wait_for_service(service);
        }
    } else if (eq(kind, "shell")) {
        (void)vfs_spawn_program(path, SHELL_PRIORITY);
    }
}

static void parse_boot_manifest(const char *buf, uint64_t size) {
    uint64_t pos = 0;
    while (pos < size && buf[pos]) {
        while (pos < size && (buf[pos] == '\n' || buf[pos] == '\r' ||
                              buf[pos] == ' ' || buf[pos] == '\t')) {
            pos++;
        }
        if (pos >= size || !buf[pos]) {
            break;
        }

        char kind[16];
        char path[64];
        uint32_t k = 0;
        while (pos < size && buf[pos] && buf[pos] != ' ' && buf[pos] != '\t' &&
               buf[pos] != '\n' && buf[pos] != '\r') {
            if (k + 1 < sizeof(kind)) {
                kind[k++] = buf[pos];
            }
            pos++;
        }
        kind[k] = '\0';

        while (pos < size && (buf[pos] == ' ' || buf[pos] == '\t')) {
            pos++;
        }

        uint32_t p = 0;
        while (pos < size && buf[pos] && buf[pos] != '\n' && buf[pos] != '\r') {
            if (p + 1 < sizeof(path)) {
                path[p++] = buf[pos];
            }
            pos++;
        }
        path[p] = '\0';

        if (kind[0] && path[0] && kind[0] != '#') {
            launch_manifest_entry(kind, path);
        }
    }
}

static void launch_manifest(void) {
    vfs_file_info_t info = vfs_open_file("/etc/boot.txt");
    if (info.status < 0 || info.size == 0) {
        return;
    }

    char *buf = (char *)sys_mmap(PAGE_BYTES);
    if (!buf) {
        vfs_close_handle(info.handle);
        return;
    }
    for (uint32_t i = 0; i < PAGE_BYTES; i++) {
        buf[i] = 0;
    }

    int64_t got = vfs_read_handle_page(info.handle, 0, buf);
    vfs_close_handle(info.handle);
    if (got > 0) {
        parse_boot_manifest(buf, (uint64_t)got);
    }
    sys_munmap(buf, PAGE_BYTES);
}

void _start(void) {
    spawn_initrd_service("ns.elf");
    spawn_initrd_service("block.elf");
    spawn_initrd_service("fs.elf");

    while (ensure_vfs_bound() < 0) {
        sys_sleep(50);
    }

    launch_manifest();

    while (1) {
        sys_sleep(1000);
    }
}
