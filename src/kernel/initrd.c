#include "initrd.h"
#include "log.h"

static const uint8_t *initrd_base = 0;
static uint64_t initrd_size = 0;
static const initrd_header_t *initrd_header = 0;
static const initrd_entry_t *initrd_entries = 0;

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int initrd_init(const void *data, uint64_t size) {
    if (!data || size < sizeof(initrd_header_t)) {
        LOG_FAIL("BOOTFS: Missing image.");
        return -1;
    }

    const initrd_header_t *header = (const initrd_header_t *)data;
    if (header->magic != INITRD_MAGIC || header->version != INITRD_VERSION) {
        LOG_FAIL("BOOTFS: Bad header.");
        return -1;
    }

    uint64_t table_size = sizeof(initrd_header_t) +
                          ((uint64_t)header->file_count * sizeof(initrd_entry_t));
    if (header->header_size < table_size || header->header_size > size) {
        LOG_FAIL("BOOTFS: Bad table size.");
        return -1;
    }

    const initrd_entry_t *entries =
        (const initrd_entry_t *)((const uint8_t *)data + sizeof(initrd_header_t));
    for (uint32_t i = 0; i < header->file_count; i++) {
        if (entries[i].offset > size || entries[i].size > size - entries[i].offset) {
            LOG_FAIL("BOOTFS: File outside image.");
            return -1;
        }
    }

    initrd_base = (const uint8_t *)data;
    initrd_size = size;
    initrd_header = header;
    initrd_entries = entries;

    LOG_DEBUG_HEX("BOOTFS: Loaded archive bytes: ", initrd_size);
    LOG_OK_HEX("BOOTFS: File count: ", header->file_count);
    return 0;
}

int initrd_find(const char *name, const uint8_t **data, uint64_t *size) {
    if (!initrd_header || !name || !data || !size) {
        return -1;
    }

    for (uint32_t i = 0; i < initrd_header->file_count; i++) {
        if (streq(name, initrd_entries[i].name)) {
            *data = initrd_base + initrd_entries[i].offset;
            *size = initrd_entries[i].size;
            return 0;
        }
    }

    return -1;
}

int initrd_find_index(const char *name, uint32_t *index) {
    if (!initrd_header || !name || !index) {
        return -1;
    }

    for (uint32_t i = 0; i < initrd_header->file_count; i++) {
        if (streq(name, initrd_entries[i].name)) {
            *index = i;
            return 0;
        }
    }

    return -1;
}

uint32_t initrd_file_count(void) {
    if (!initrd_header) {
        return 0;
    }
    return initrd_header->file_count;
}

const initrd_entry_t *initrd_get_entry(uint32_t index) {
    if (!initrd_header || index >= initrd_header->file_count) {
        return 0;
    }
    return &initrd_entries[index];
}

int initrd_get_file(uint32_t index, const uint8_t **data, uint64_t *size) {
    if (!initrd_header || !data || !size || index >= initrd_header->file_count) {
        return -1;
    }

    *data = initrd_base + initrd_entries[index].offset;
    *size = initrd_entries[index].size;
    return 0;
}

const initrd_header_t *initrd_get_header(void) {
    return initrd_header;
}
