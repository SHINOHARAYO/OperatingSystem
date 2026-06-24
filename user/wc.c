#include "lib.h"
#include "fcntl.h"
#include "unistd.h"

static int count_fd(int fd, uint64_t *lines, uint64_t *words, uint64_t *bytes) {
    char buffer[512];
    int in_word = 0;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got == 0) return 0;
        if (got < 0) return -1;
        *bytes += (uint64_t)got;
        for (ssize_t i = 0; i < got; i++) {
            if (buffer[i] == '\n') (*lines)++;
            int space = isspace((unsigned char)buffer[i]);
            if (!space && !in_word) (*words)++;
            in_word = !space;
        }
    }
}

int main(int argc, char **argv) {
    uint64_t lines = 0, words = 0, bytes = 0;
    int fd = 0;
    if (argc > 2) return 1;
    if (argc == 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) return 1;
    }
    int result = count_fd(fd, &lines, &words, &bytes);
    if (fd != 0) close(fd);
    if (result < 0) return 1;
    char output[96];
    int length = snprintf(output, sizeof(output), "%lu %lu %lu\n",
                          lines, words, bytes);
    return length > 0 && write(1, output, (size_t)length) == length ? 0 : 1;
}
