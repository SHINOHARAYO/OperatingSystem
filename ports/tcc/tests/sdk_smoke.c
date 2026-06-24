#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int neptune_tcc_sdk_smoke(const char *input) {
    char copy[32];
    uint32_t value = (uint32_t)strtoul(input, 0, 10);
    strcpy(copy, "sdk");
    printf("%s:%u\n", copy, value);
    return strcmp(copy, "sdk") == 0 ? (int)value : -1;
}
