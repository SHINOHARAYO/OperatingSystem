#pragma once

#include "display_boot.h"

#define DISPLAY_REQ_INFO      1ULL
#define DISPLAY_REQ_CLEAR     2ULL
#define DISPLAY_REQ_FILL_RECT 3ULL
#define DISPLAY_REQ_REGISTER_BUFFER 4ULL
#define DISPLAY_REQ_BLIT_BUFFER     5ULL
#define DISPLAY_REQ_CAPS            6ULL
#define DISPLAY_REQ_STATS           7ULL
#define DISPLAY_REQ_RESET_STATS     8ULL
#define DISPLAY_REQ_BIND_PRESENT_QUEUE 9ULL

#define DISPLAY_MAX_BUFFERS 8U

#define DISPLAY_PRESENT_QUEUE_MAGIC 0x4453505245535155ULL
#define DISPLAY_PRESENT_QUEUE_VERSION 1ULL
#define DISPLAY_PRESENT_QUEUE_CAPACITY 128U

#define DISPLAY_BACKEND_NONE        0ULL
#define DISPLAY_BACKEND_RAMFB       1ULL
#define DISPLAY_BACKEND_VIRTIO_GPU  2ULL

#define DISPLAY_FEATURE_FILL_RECT       (1ULL << 0)
#define DISPLAY_FEATURE_REGISTER_BUFFER (1ULL << 1)
#define DISPLAY_FEATURE_BLIT_BUFFER     (1ULL << 2)
#define DISPLAY_FEATURE_CPU_RGB888      (1ULL << 3)
#define DISPLAY_FEATURE_SYNC_PRESENT    (1ULL << 4)
#define DISPLAY_FEATURE_PRESENT_QUEUE   (1ULL << 5)

static inline uint64_t display_pack_pair(uint32_t a, uint32_t b) {
    return ((uint64_t)a << 32) | b;
}

typedef struct {
    uint32_t buffer_id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} display_present_cmd_t;

typedef struct {
    volatile uint64_t magic;
    volatile uint64_t version;
    volatile uint32_t capacity;
    volatile uint32_t read_index;
    volatile uint32_t write_index;
    volatile uint32_t dropped;
    display_present_cmd_t commands[DISPLAY_PRESENT_QUEUE_CAPACITY];
} display_present_queue_t;
