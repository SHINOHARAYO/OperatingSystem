#include "lib.h"
#include "fcntl.h"
#include "unistd.h"

static void expect(int condition, const char *name) {
    if (!condition) {
        printf("argfdtest: FAIL %s\n", name);
        return;
    }
}

int main(int argc, char **argv) {
    int ok = argc == 3 && argv &&
             strcmp(argv[0], "argfdtest.elf") == 0 &&
             strcmp(argv[1], "alpha") == 0 &&
             strcmp(argv[2], "beta") == 0;
    if (!ok) {
        printf("argfdtest: FAIL argv argc=%d\n", argc);
        return 1;
    }

    char boot[32];
    int fd = open("/etc/boot.txt", O_RDONLY);
    if (fd < 0 || read(fd, boot, sizeof(boot) - 1) <= 0 || close(fd) < 0) {
        printf("argfdtest: FAIL read boot manifest\n");
        return 1;
    }

    const char text[] = "fd api works";
    fd = open("argfdtest.tmp", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || write(fd, text, sizeof(text) - 1) != sizeof(text) - 1 ||
        close(fd) < 0) {
        printf("argfdtest: FAIL write\n");
        return 1;
    }

    char verify[32];
    memset(verify, 0, sizeof(verify));
    fd = open("argfdtest.tmp", O_RDONLY);
    if (fd < 0 || lseek(fd, 3, SEEK_SET) != 3 ||
        read(fd, verify, sizeof(text) - 4) != sizeof(text) - 4 ||
        close(fd) < 0 || strcmp(verify, "api works") != 0) {
        printf("argfdtest: FAIL seek/read\n");
        return 1;
    }
    (void)vfs_delete_file("argfdtest.tmp");

    expect(1, "complete");
    printf("argfdtest: ok argc=%d argv=%s,%s\n", argc, argv[1], argv[2]);
    return 0;
}
