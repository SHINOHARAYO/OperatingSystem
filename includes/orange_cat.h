#pragma once

#include <stdint.h>

#define OCAP_INITIAL_CAPS 32

typedef enum {
    OCAP_NONE = 0,
    OCAP_ENDPOINT = 1,
    OCAP_EXEC = 2,
    OCAP_FRAME = 3,
    OCAP_VMA = 4,
    OCAP_REPLY = 5,
    OCAP_FILE = 6,
    OCAP_DMA = 7
} ocap_type_t;

#define OCAP_RIGHT_READ     (1ULL << 0)
#define OCAP_RIGHT_WRITE    (1ULL << 1)
#define OCAP_RIGHT_CALL     (1ULL << 2)
#define OCAP_RIGHT_REPLY    (1ULL << 3)
#define OCAP_RIGHT_SPAWN    (1ULL << 4)
#define OCAP_RIGHT_MAP      (1ULL << 5)
#define OCAP_RIGHT_SHARE    (1ULL << 6)
#define OCAP_RIGHT_TRANSFER (1ULL << 7)
#define OCAP_RIGHT_LEND     (1ULL << 8)
#define OCAP_RIGHT_REVOKE   (1ULL << 9)
#define OCAP_RIGHT_DMA      (1ULL << 10)

#define OCAP_FLAG_VFS_EXEC  (1ULL << 0)

typedef struct {
    uint32_t type;
    uint32_t object_id;
    uint64_t rights;
    uint64_t flags;
} ocap_t;

typedef struct {
    ocap_t *entries;
    uint32_t capacity;
    uint32_t pages;
} orange_cat_table_t;

typedef void (*ocap_release_hook_t)(const ocap_t *cap);

void ocap_set_release_hook(ocap_release_hook_t hook);
int ocap_ensure(orange_cat_table_t *table, uint32_t min_capacity);
int ocap_install(orange_cat_table_t *table, uint32_t type, uint32_t object_id,
                 uint64_t rights, uint64_t flags);
int ocap_install_at(orange_cat_table_t *table, uint32_t slot, uint32_t type,
                    uint32_t object_id, uint64_t rights, uint64_t flags);
int ocap_read_slot(const orange_cat_table_t *table, uint32_t slot, ocap_t *out);
int ocap_lookup(const orange_cat_table_t *table, uint32_t slot, uint32_t type,
                uint64_t required_rights, ocap_t *out);
int ocap_copy(const orange_cat_table_t *src, orange_cat_table_t *dst,
              uint32_t src_slot, uint64_t required_rights,
              uint32_t *dst_slot);
void ocap_clear(orange_cat_table_t *table);
void ocap_release_table(orange_cat_table_t *table);
void ocap_revoke_slot(orange_cat_table_t *table, uint32_t slot);
