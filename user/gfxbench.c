#include "lib.h"
#include "ns_proto.h"
#include "compositor_proto.h"

#define PAGE_SIZE 4096ULL
#define SURFACE_COUNT 4U
#define BENCH_FRAMES 240U
#define FRAME_SLEEP_MS 4ULL

typedef struct {
    uint32_t *pixels;
    uint64_t size;
    int mem_cap;
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;
    int32_t dx;
    int32_t dy;
    uint32_t color;
} bench_surface_t;

static uint64_t round_pages(uint64_t size) {
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static void fill_rect(uint32_t *pixels, uint32_t stride,
                      uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height,
                      uint32_t color) {
    for (uint32_t row = 0; row < height; row++) {
        uint32_t *dst = pixels + (uint64_t)(y + row) * stride + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = color;
        }
    }
}

static void draw_surface(bench_surface_t *surface, uint32_t frame) {
    fill_rect(surface->pixels, surface->width, 0, 0,
              surface->width, surface->height, 0x07111F);
    for (uint32_t y = 0; y < surface->height; y += 24) {
        fill_rect(surface->pixels, surface->width, 0, y,
                  surface->width, 2, 0x0F2438);
    }
    for (uint32_t x = 0; x < surface->width; x += 24) {
        fill_rect(surface->pixels, surface->width, x, 0,
                  2, surface->height, 0x0F2438);
    }

    uint32_t box = surface->height / 5;
    if (box < 18) box = 18;
    if (box > 80) box = 80;
    uint32_t travel_x = surface->width > box ? surface->width - box : 1;
    uint32_t travel_y = surface->height > box ? surface->height - box : 1;
    uint32_t x = (frame * 9U + surface->color) % travel_x;
    uint32_t y = (frame * 5U + (surface->color >> 8)) % travel_y;
    fill_rect(surface->pixels, surface->width, x, y, box, box,
              surface->color);
    fill_rect(surface->pixels, surface->width,
              surface->width / 12, surface->height / 2,
              surface->width * 5 / 6, 3, 0xE5EEF7);
}

static int create_surface(int compositor_cap, bench_surface_t *surface) {
    surface->size = round_pages((uint64_t)surface->width *
                                surface->height * sizeof(uint32_t));
    surface->pixels = (uint32_t *)sys_mmap(surface->size);
    if (!surface->pixels) {
        return -1;
    }
    draw_surface(surface, 0);

    surface->mem_cap = sys_mem_export(surface->pixels, surface->size,
                                      MEM_RIGHT_READ | MEM_RIGHT_WRITE |
                                      MEM_RIGHT_LEND);
    if (surface->mem_cap < 0) {
        sys_munmap(surface->pixels, surface->size);
        return -1;
    }

    uint64_t payload[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)surface->mem_cap,
        [1] = 0,
        [2] = surface->size,
        [3] = MEM_RIGHT_READ | IPC_MEM_MODE_LEND,
        [4] = COMPOSITOR_REQ_CREATE_SURFACE,
        [5] = display_pack_pair(surface->width, surface->height),
        [6] = display_pack_pair(surface->x, surface->y),
        [7] = surface->width
    };
    ipc_msg_t created =
        sys_ipc_call((uint32_t)compositor_cap, IPC_FLAG_MEM, 64, payload);
    if (created.status < 0 || created.payload[0] == 0) {
        sys_mem_revoke((uint32_t)surface->mem_cap);
        sys_munmap(surface->pixels, surface->size);
        return -1;
    }
    surface->surface_id = (uint32_t)created.payload[0];
    return 0;
}

static void destroy_surface(int compositor_cap, bench_surface_t *surface) {
    if (surface->surface_id) {
        uint64_t payload[IPC_INLINE_WORDS] = {
            COMPOSITOR_REQ_DESTROY_SURFACE, surface->surface_id
        };
        (void)sys_ipc_call((uint32_t)compositor_cap, 0, 16, payload);
    }
    if (surface->mem_cap >= 0) {
        (void)sys_mem_revoke((uint32_t)surface->mem_cap);
    }
    if (surface->pixels) {
        (void)sys_munmap(surface->pixels, surface->size);
    }
}

static void reset_compositor_stats(int compositor_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_RESET_STATS};
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 8, payload);
}

static void reset_display_stats(int display_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {DISPLAY_REQ_RESET_STATS};
    (void)sys_ipc_call((uint32_t)display_cap, 0, 8, payload);
}

static ipc_msg_t compositor_stats(int compositor_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_STATS};
    return sys_ipc_call((uint32_t)compositor_cap, 0, 8, payload);
}

static ipc_msg_t display_stats(int display_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {DISPLAY_REQ_STATS};
    return sys_ipc_call((uint32_t)display_cap, 0, 8, payload);
}

static void damage_surface(int compositor_cap, bench_surface_t *surface) {
    uint64_t payload[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DAMAGE_SURFACE,
        surface->surface_id,
        display_pack_pair(0, 0),
        display_pack_pair(surface->width, surface->height)
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 32, payload);
}

static void move_surface(int compositor_cap, bench_surface_t *surface,
                         uint32_t screen_width, uint32_t screen_height) {
    int32_t nx = (int32_t)surface->x + surface->dx;
    int32_t ny = (int32_t)surface->y + surface->dy;
    uint32_t max_x = screen_width > surface->width ?
                     screen_width - surface->width : 0;
    uint32_t max_y = screen_height > surface->height ?
                     screen_height - surface->height : 0;
    if (nx < 0) {
        nx = 0;
        surface->dx = -surface->dx;
    }
    if (ny < 0) {
        ny = 0;
        surface->dy = -surface->dy;
    }
    if ((uint32_t)nx > max_x) {
        nx = (int32_t)max_x;
        surface->dx = -surface->dx;
    }
    if ((uint32_t)ny > max_y) {
        ny = (int32_t)max_y;
        surface->dy = -surface->dy;
    }
    surface->x = (uint32_t)nx;
    surface->y = (uint32_t)ny;

    uint64_t payload[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_MOVE_SURFACE,
        surface->surface_id,
        display_pack_pair(surface->x, surface->y)
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 24, payload);
}

void _start(void) {
    int compositor_cap = ns_resolve("compositor");
    int display_cap = ns_resolve("display");
    if (compositor_cap < 0 || display_cap < 0) {
        printf("gfxbench: graphics services unavailable\n");
        sys_exit(1);
    }

    uint64_t info_request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_INFO};
    ipc_msg_t info =
        sys_ipc_call((uint32_t)compositor_cap, 0, 8, info_request);
    if (info.status < 0) {
        printf("gfxbench: compositor info failed\n");
        sys_exit(1);
    }
    uint32_t screen_width = (uint32_t)(info.payload[0] >> 32);
    uint32_t screen_height = (uint32_t)info.payload[0];

    uint32_t w = screen_width / 4;
    uint32_t h = screen_height / 5;
    if (w < 180) w = 180;
    if (h < 120) h = 120;
    if (w > screen_width / 2) w = screen_width / 2;
    if (h > screen_height / 2) h = screen_height / 2;

    bench_surface_t surfaces[SURFACE_COUNT] = {
        {0, 0, -1, 0, w, h, screen_width / 12, screen_height / 10, 7, 4, 0x12B8A6},
        {0, 0, -1, 0, w, h, screen_width / 3, screen_height / 5, -6, 5, 0xF4C95D},
        {0, 0, -1, 0, w, h, screen_width / 2, screen_height / 3, 5, -4, 0xEF6F6C},
        {0, 0, -1, 0, w, h, screen_width / 5, screen_height / 2, -4, -5, 0x5DADE2}
    };

    for (uint32_t i = 0; i < SURFACE_COUNT; i++) {
        if (create_surface(compositor_cap, &surfaces[i]) < 0) {
            printf("gfxbench: failed to create surface %u\n", i);
            for (uint32_t j = 0; j < i; j++) {
                destroy_surface(compositor_cap, &surfaces[j]);
            }
            sys_exit(1);
        }
    }

    reset_compositor_stats(compositor_cap);
    reset_display_stats(display_cap);

    uint64_t start_ms = sys_uptime_ms();
    for (uint32_t frame = 0; frame < BENCH_FRAMES; frame++) {
        for (uint32_t i = 0; i < SURFACE_COUNT; i++) {
            draw_surface(&surfaces[i], frame + i * 17U);
            damage_surface(compositor_cap, &surfaces[i]);
            if ((frame & 1U) == 0) {
                move_surface(compositor_cap, &surfaces[i],
                             screen_width, screen_height);
            }
        }
        sys_sleep(FRAME_SLEEP_MS);
    }
    uint64_t elapsed_ms = sys_uptime_ms() - start_ms;
    if (elapsed_ms == 0) {
        elapsed_ms = 1;
    }

    ipc_msg_t c = compositor_stats(compositor_cap);
    ipc_msg_t d = display_stats(display_cap);
    uint64_t fps_x10 = (uint64_t)BENCH_FRAMES * 10000ULL / elapsed_ms;

    printf("gfxbench: mode=%ux%u surfaces=%u frames=%u elapsed=%lums fps=%lu.%lu\n",
           screen_width, screen_height, SURFACE_COUNT, BENCH_FRAMES,
           elapsed_ms, fps_x10 / 10ULL, fps_x10 % 10ULL);
    if (c.status == 0) {
        uint64_t presents = c.payload[5] ? c.payload[5] : 1ULL;
        printf("gfxbench: compositor damage=%lu queued=%lu merged=%lu collapses=%lu\n",
               c.payload[0], c.payload[1], c.payload[2], c.payload[3]);
        printf("gfxbench: compositor flushes=%lu presents=%lu rects/flush=%lu full=%lu drops=%lu\n",
               c.payload[4], c.payload[5], c.payload[5] / (c.payload[4] ? c.payload[4] : 1ULL),
               c.payload[7], c.payload[8]);
        printf("gfxbench: compositor presents/sec=%lu\n",
               presents * 1000ULL / elapsed_ms);
    }
    if (d.status == 0) {
        uint64_t mb_x10 = d.payload[9] * 10ULL / (1024ULL * 1024ULL);
        uint64_t mbps_x10 = d.payload[9] * 10000ULL /
                            (1024ULL * 1024ULL * elapsed_ms);
        printf("gfxbench: display blits=%lu queue=%lu errors=%lu gpu-cmd=%lu transfers=%lu flushes=%lu\n",
               d.payload[2], d.payload[14], d.payload[3],
               d.payload[6], d.payload[7], d.payload[8]);
        printf("gfxbench: gpu bytes=%lu.%luMiB throughput=%lu.%luMiB/s batches=%lu max-dirty=%lu\n",
               mb_x10 / 10ULL, mb_x10 % 10ULL,
               mbps_x10 / 10ULL, mbps_x10 % 10ULL,
               d.payload[12], d.payload[13]);
    }

    for (uint32_t i = 0; i < SURFACE_COUNT; i++) {
        destroy_surface(compositor_cap, &surfaces[i]);
    }
    sys_exit(0);
}
