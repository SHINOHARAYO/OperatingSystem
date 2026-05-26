#pragma once

#include <stdint.h>
#include <stddef.h>

#define INITRD_MAGIC 0x4452494E
#define INITRD_VERSION 1
#define INITRD_NAME_LEN 56

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t header_size;
} initrd_header_t;

typedef struct {
    char name[INITRD_NAME_LEN];
    uint64_t offset;
    uint64_t size;
} initrd_entry_t;

int initrd_init(const void *data, uint64_t size);
int initrd_find(const char *name, const uint8_t **data, uint64_t *size);
int initrd_find_index(const char *name, uint32_t *index);
uint32_t initrd_file_count(void);
const initrd_entry_t *initrd_get_entry(uint32_t index);
int initrd_get_file(uint32_t index, const uint8_t **data, uint64_t *size);
const initrd_header_t *initrd_get_header(void);
