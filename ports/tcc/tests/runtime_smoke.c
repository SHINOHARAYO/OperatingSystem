#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(int code) {
    fprintf(stderr, "tcc-runtime: failed %d\n", code);
    return code;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "alpha") != 0 ||
        strcmp(argv[2], "beta") != 0) {
        return fail(10);
    }

    char formatted[32];
    if (snprintf(formatted, sizeof(formatted), "%s:%d", argv[1], 42) != 8 ||
        strcmp(formatted, "alpha:42") != 0) {
        return fail(11);
    }

    char *text = malloc(8);
    if (!text) return fail(12);
    strcpy(text, "hello");
    text = realloc(text, 32);
    if (!text || strcmp(text, "hello") != 0) return fail(13);

    unsigned char *zeroes = calloc(8, 1);
    if (!zeroes) return fail(14);
    for (int i = 0; i < 8; i++) {
        if (zeroes[i] != 0) return fail(15);
    }

    int fd = open("tcc-runtime.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || write(fd, text, 5) != 5 || close(fd) != 0) return fail(16);
    fd = open("tcc-runtime.txt", O_RDONLY);
    char readback[8] = {0};
    if (fd < 0 || read(fd, readback, 5) != 5 || close(fd) != 0 ||
        strcmp(readback, "hello") != 0) {
        return fail(17);
    }

    free(zeroes);
    free(text);
    if (fprintf(stdout, "tcc-runtime: ok %s %s\n", argv[1], argv[2]) < 0 ||
        fflush(stdout) != 0) {
        return fail(18);
    }
    return 0;
}
