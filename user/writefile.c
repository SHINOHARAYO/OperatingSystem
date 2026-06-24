#include "lib.h"
#include "fcntl.h"
#include "unistd.h"

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;
    char buffer[512];
    int result = 0;
    for (;;) {
        ssize_t got = read(0, buffer, sizeof(buffer));
        if (got == 0) break;
        if (got < 0 || write(fd, buffer, (size_t)got) != got) {
            result = 1;
            break;
        }
    }
    if (close(fd) < 0) result = 1;
    return result;
}
