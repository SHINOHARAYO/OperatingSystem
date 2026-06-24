#include "lib.h"
#include "ns_proto.h"
#include "compositor_proto.h"

#define PAGE_SIZE 4096ULL
#define BALL_COUNT 8U
#define DEMO_FRAMES 300U
#define FRAME_SLEEP_MS 8ULL

typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    uint32_t size;
    uint32_t color;
} ball_t;

static uint64_t round_pages(uint64_t size) {
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static void fill_rect(uint32_t *pixels, uint32_t stride, uint32_t x,
                      uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color) {
    for (uint32_t row = 0; row < height; row++) {
        uint32_t *dst = pixels + (uint64_t)(y + row) * stride + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = color;
        }
    }
}

static void draw_background(uint32_t *pixels, uint32_t width, uint32_t height) {
    fill_rect(pixels, width, 0, 0, width, height, 0x06111D);
    for (uint32_t y = 0; y < height; y += 16) {
        fill_rect(pixels, width, 0, y, width, 1, 0x0A1A2A);
    }
    for (uint32_t x = 0; x < width; x += 16) {
        fill_rect(pixels, width, x, 0, 1, height, 0x0A1A2A);
    }
}

static void restore_background_rect(uint32_t *pixels, uint32_t width,
                                    uint32_t height, uint32_t x, uint32_t y,
                                    uint32_t rect_width,
                                    uint32_t rect_height) {
    if (x >= width || y >= height) {
        return;
    }
    if (rect_width > width - x) rect_width = width - x;
    if (rect_height > height - y) rect_height = height - y;
    fill_rect(pixels, width, x, y, rect_width, rect_height, 0x06111D);
    for (uint32_t row = y; row < y + rect_height; row++) {
        if ((row & 15U) == 0) {
            fill_rect(pixels, width, x, row, rect_width, 1, 0x0A1A2A);
        }
    }
    for (uint32_t col = x; col < x + rect_width; col++) {
        if ((col & 15U) == 0) {
            fill_rect(pixels, width, col, y, 1, rect_height, 0x0A1A2A);
        }
    }
}

static int send_damage(int compositor_cap, uint32_t surface_id,
                       uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height) {
    uint64_t payload[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DAMAGE_SURFACE,
        surface_id,
        display_pack_pair(x, y),
        display_pack_pair(width, height)
    };
    return sys_ipc_call((uint32_t)compositor_cap, 0, 32, payload).status;
}

static int move_surface(int compositor_cap, uint32_t surface_id,
                        uint32_t x, uint32_t y) {
    uint64_t payload[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_MOVE_SURFACE,
        surface_id,
        display_pack_pair(x, y)
    };
    return sys_ipc_call((uint32_t)compositor_cap, 0, 24, payload).status;
}

static ipc_msg_t compositor_stats(int compositor_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_STATS};
    return sys_ipc_call((uint32_t)compositor_cap, 0, 8, payload);
}

static void reset_stats(int compositor_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_RESET_STATS};
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 8, payload);
}

void _start(void) {
    int compositor_cap = ns_resolve("compositor");
    if (compositor_cap < 0) {
        printf("gfxdemo: compositor unavailable\n");
        sys_exit(1);
    }

    uint64_t info_request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_INFO};
    ipc_msg_t info = sys_ipc_call((uint32_t)compositor_cap, 0, 8, info_request);
    if (info.status < 0) {
        printf("gfxdemo: graphical display unavailable\n");
        sys_exit(1);
    }

    uint32_t screen_width = (uint32_t)(info.payload[0] >> 32);
    uint32_t screen_height = (uint32_t)info.payload[0];
    uint32_t width = screen_width * 2 / 3;
    uint32_t height = screen_height * 2 / 3;
    if (width < 160) width = screen_width;
    if (height < 120) height = screen_height;
    if (width > screen_width) width = screen_width;
    if (height > screen_height) height = screen_height;
    if (width < 64 || height < 64) {
        printf("gfxdemo: display is too small\n");
        sys_exit(1);
    }

    uint64_t size = round_pages((uint64_t)width * height * sizeof(uint32_t));
    uint32_t *pixels = (uint32_t *)sys_mmap(size);
    if (!pixels) {
        printf("gfxdemo: surface allocation failed\n");
        sys_exit(1);
    }
    draw_background(pixels, width, height);

    ball_t balls[BALL_COUNT] = {
        {12, 10, 3, 2, 22, 0x12B8A6},
        {70, 22, -2, 3, 18, 0xF4C95D},
        {130, 58, 4, -2, 26, 0xEF6F6C},
        {190, 84, -3, -3, 20, 0x5DADE2},
        {38, 120, 2, -4, 16, 0xB47CFF},
        {110, 150, -4, 2, 24, 0x7BDD91},
        {210, 36, 3, 4, 14, 0xF4F7FA},
        {250, 112, -2, -3, 28, 0xFF9F1C}
    };
    for (uint32_t i = 0; i < BALL_COUNT; i++) {
        balls[i].x %= (int)(width - balls[i].size);
        balls[i].y %= (int)(height - balls[i].size);
        fill_rect(pixels, width, (uint32_t)balls[i].x, (uint32_t)balls[i].y,
                  balls[i].size, balls[i].size, balls[i].color);
    }

    int mem_cap = sys_mem_export(pixels, size, MEM_RIGHT_READ |
                                               MEM_RIGHT_WRITE |
                                               MEM_RIGHT_LEND);
    if (mem_cap < 0) {
        sys_munmap(pixels, size);
        printf("gfxdemo: surface export failed\n");
        sys_exit(1);
    }

    uint32_t surface_x = (screen_width - width) / 2;
    uint32_t surface_y = (screen_height - height) / 2;
    uint64_t create[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)mem_cap,
        [1] = 0,
        [2] = size,
        [3] = MEM_RIGHT_READ | IPC_MEM_MODE_LEND,
        [4] = COMPOSITOR_REQ_CREATE_SURFACE,
        [5] = display_pack_pair(width, height),
        [6] = display_pack_pair(surface_x, surface_y),
        [7] = width
    };
    ipc_msg_t created =
        sys_ipc_call((uint32_t)compositor_cap, IPC_FLAG_MEM, 64, create);
    if (created.status < 0 || created.payload[0] == 0) {
        sys_mem_revoke((uint32_t)mem_cap);
        sys_munmap(pixels, size);
        printf("gfxdemo: compositor rejected surface\n");
        sys_exit(1);
    }
    uint32_t surface_id = (uint32_t)created.payload[0];

    reset_stats(compositor_cap);
    uint64_t start_ms = sys_uptime_ms();
    for (uint32_t frame = 0; frame < DEMO_FRAMES; frame++) {
        for (uint32_t i = 0; i < BALL_COUNT; i++) {
            ball_t *ball = &balls[i];
            uint32_t old_x = (uint32_t)ball->x;
            uint32_t old_y = (uint32_t)ball->y;
            restore_background_rect(pixels, width, height, old_x, old_y,
                                    ball->size, ball->size);

            ball->x += ball->dx;
            ball->y += ball->dy;
            if (ball->x < 0) {
                ball->x = 0;
                ball->dx = -ball->dx;
            }
            if (ball->y < 0) {
                ball->y = 0;
                ball->dy = -ball->dy;
            }
            if ((uint32_t)ball->x + ball->size >= width) {
                ball->x = (int)(width - ball->size);
                ball->dx = -ball->dx;
            }
            if ((uint32_t)ball->y + ball->size >= height) {
                ball->y = (int)(height - ball->size);
                ball->dy = -ball->dy;
            }

            fill_rect(pixels, width, (uint32_t)ball->x, (uint32_t)ball->y,
                      ball->size, ball->size, ball->color);
            (void)send_damage(compositor_cap, surface_id, old_x, old_y,
                              ball->size, ball->size);
            (void)send_damage(compositor_cap, surface_id, (uint32_t)ball->x,
                              (uint32_t)ball->y, ball->size, ball->size);
        }

        if ((frame % 45U) == 44U && screen_width > width &&
            screen_height > height) {
            uint32_t travel_x = screen_width - width;
            uint32_t travel_y = screen_height - height;
            surface_x = (frame * 7U) % (travel_x + 1U);
            surface_y = (frame * 5U) % (travel_y + 1U);
            (void)move_surface(compositor_cap, surface_id, surface_x, surface_y);
        }
        sys_sleep(FRAME_SLEEP_MS);
    }
    uint64_t elapsed = sys_uptime_ms() - start_ms;

    ipc_msg_t stats = compositor_stats(compositor_cap);
    if (stats.status == 0) {
        printf("gfxdemo: frames=%u elapsed=%lums\n", DEMO_FRAMES, elapsed);
        printf("gfxdemo: damage=%lu queued=%lu merged=%lu collapses=%lu\n",
               stats.payload[0], stats.payload[1], stats.payload[2],
               stats.payload[3]);
        printf("gfxdemo: flushes=%lu presented=%lu max-dirty=%lu full=%lu\n",
               stats.payload[4], stats.payload[5], stats.payload[6],
               stats.payload[7]);
    }

    uint64_t destroy[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DESTROY_SURFACE, surface_id
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 16, destroy);
    (void)sys_mem_revoke((uint32_t)mem_cap);
    (void)sys_munmap(pixels, size);
    sys_exit(0);
}
