#include "lib.h"
#include "fcntl.h"
#include "unistd.h"

static int copy_fd(int fd) {
    char buffer[512];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got == 0) return 0;
        if (got < 0 || write(1, buffer, (size_t)got) != got) return -1;
    }
}

int main(int argc, char **argv) {
    if (argc == 1) return copy_fd(0) < 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) return 1;
        int result = copy_fd(fd);
        close(fd);
        if (result < 0) return 1;
    }
    return 0;
}
