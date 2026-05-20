#pragma once

#include <stdint.h>

#define FRAME_FLAG_USER     (1ULL << 0)
#define FRAME_FLAG_SHARED   (1ULL << 1)
#define FRAME_FLAG_PINNED   (1ULL << 2)

typedef struct {
    uint64_t paddr;
    uint32_t refcount;
    uint32_t owner_tid;
    uint64_t flags;
} frame_t;

int frame_init(void);
frame_t *frame_alloc(uint32_t owner_tid, uint64_t flags);
frame_t *frame_from_paddr(uint64_t paddr);
int frame_ref(uint64_t paddr);
int frame_unref(uint64_t paddr);
