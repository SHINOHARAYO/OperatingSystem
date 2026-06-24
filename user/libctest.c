#include "lib.h"

static void expect(int condition, const char *name) {
    if (!condition) {
        printf("libctest: FAIL %s\n", name);
        sys_exit(1);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buf[64];
    char overlap[16];

    memset(buf, 'A', sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    expect(strlen(buf) == sizeof(buf) - 1, "strlen/memset");

    memcpy(buf, "hello", 6);
    expect(strcmp(buf, "hello") == 0, "memcpy/strcmp");
    expect(strncmp(buf, "he", 2) == 0, "strncmp prefix");
    expect(strchr(buf, 'l') == buf + 2, "strchr");

    strcpy(overlap, "abcdef");
    memmove(overlap + 2, overlap, 7);
    expect(strcmp(overlap, "ababcdef") == 0, "memmove overlap right");
    memmove(overlap, overlap + 2, 7);
    expect(strcmp(overlap, "abcdef") == 0, "memmove overlap left");

    memset(buf, 0, sizeof(buf));
    strncpy(buf, "neptune", 4);
    expect(strcmp(buf, "nept") == 0, "strncpy trunc");

    expect(atoi(" -42") == -42, "atoi negative");
    expect(isdigit('8') && !isdigit('x'), "isdigit");
    expect(isalpha('Q') && isalnum('7') && !isalnum('#'), "ctype");
    expect(tolower('Z') == 'z' && toupper('q') == 'Q', "case");

    int n = snprintf(buf, sizeof(buf), "%s %d %u %x %lu",
                     "fmt", -7, 42U, 0x2aU, 99ULL);
    expect(n == 15, "snprintf length");
    expect(strcmp(buf, "fmt -7 42 2a 99") == 0, "snprintf text");

    char *heap = (char *)malloc(12);
    expect(heap != 0, "malloc");
    strcpy(heap, "heap");
    heap = (char *)realloc(heap, 32);
    expect(heap != 0 && strcmp(heap, "heap") == 0, "realloc grow");
    free(heap);

    char *zero = (char *)calloc(8, 2);
    expect(zero != 0, "calloc alloc");
    for (uint32_t i = 0; i < 16; i++) {
        expect(zero[i] == 0, "calloc zero");
    }
    free(zero);

    printf("libctest: ok\n");
    return 0;
}
