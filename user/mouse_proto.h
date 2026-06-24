#pragma once

#define MOUSE_REQ_INFO 1ULL
#define MOUSE_REQ_READ 2ULL
#define MOUSE_REQ_STATS 3ULL
#define MOUSE_REQ_BIND_RING 4ULL

#define MOUSE_RING_MAGIC 0x4D4F55534552494EULL
#define MOUSE_RING_VERSION 1ULL
#define MOUSE_RING_CAPACITY 128U

#define MOUSE_EVENT_POINTER 1ULL

#define MOUSE_BUTTON_LEFT   (1ULL << 0)
#define MOUSE_BUTTON_RIGHT  (1ULL << 1)
#define MOUSE_BUTTON_MIDDLE (1ULL << 2)

#define MOUSE_EVENT_PACK(type, buttons) \
    ((((uint64_t)(type) & 0xFFULL) << 56) | ((uint64_t)(buttons) & 0xFFFFULL))

#define MOUSE_EVENT_TYPE(event) (((event) >> 56) & 0xFFULL)
#define MOUSE_EVENT_BUTTONS(event) ((event) & 0xFFFFULL)

static inline uint64_t mouse_pack_pair(uint32_t a, uint32_t b) {
    return ((uint64_t)a << 32) | b;
}

typedef struct {
    uint64_t event;
    uint32_t x;
    uint32_t y;
} mouse_ring_event_t;

typedef struct {
    volatile uint64_t magic;
    volatile uint64_t version;
    volatile uint32_t capacity;
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    volatile uint32_t dropped;
    mouse_ring_event_t events[MOUSE_RING_CAPACITY];
} mouse_ring_t;
