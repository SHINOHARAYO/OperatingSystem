#include "lib.h"
#include "ns_proto.h"
#include "compositor_proto.h"
#include "keyboard_proto.h"

#define PAGE_SIZE 4096ULL
#define SNAKE_COLS 24
#define SNAKE_ROWS 18
#define SNAKE_MAX (SNAKE_COLS * SNAKE_ROWS)

typedef struct {
    uint8_t x;
    uint8_t y;
} snake_point_t;

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

static int snake_contains(const snake_point_t *body, uint32_t length,
                          uint32_t x, uint32_t y) {
    for (uint32_t i = 0; i < length; i++) {
        if (body[i].x == x && body[i].y == y) {
            return 1;
        }
    }
    return 0;
}

static void place_food(snake_point_t *food, const snake_point_t *body,
                       uint32_t length, uint32_t *seed) {
    for (uint32_t attempt = 0; attempt < SNAKE_MAX; attempt++) {
        *seed = *seed * 1664525U + 1013904223U;
        uint32_t pos = *seed % SNAKE_MAX;
        uint32_t x = pos % SNAKE_COLS;
        uint32_t y = pos / SNAKE_COLS;
        if (!snake_contains(body, length, x, y)) {
            food->x = (uint8_t)x;
            food->y = (uint8_t)y;
            return;
        }
    }
}

static void render(uint32_t *pixels, const snake_point_t *body,
                   uint32_t length, snake_point_t food, uint32_t cell) {
    uint32_t width = SNAKE_COLS * cell;
    uint32_t height = SNAKE_ROWS * cell;
    uint32_t grid = cell > 20 ? 1 : 0;
    uint32_t inset = cell / 7;
    uint32_t food_inset = cell / 4;
    fill_rect(pixels, width, 0, 0, width, height, 0x07120D);

    for (uint32_t row = 0; row < SNAKE_ROWS; row++) {
        for (uint32_t col = 0; col < SNAKE_COLS; col++) {
            if ((row + col) & 1U) {
                fill_rect(pixels, width, col * cell, row * cell,
                          cell, cell, 0x09170F);
            }
            if (grid) {
                fill_rect(pixels, width, col * cell, row * cell,
                          cell, 1, 0x0C1D13);
            }
        }
    }

    fill_rect(pixels, width, food.x * cell + food_inset,
              food.y * cell + food_inset, cell - food_inset * 2,
              cell - food_inset * 2, 0xEF6F6C);
    for (uint32_t i = length; i > 0; i--) {
        snake_point_t point = body[i - 1];
        fill_rect(pixels, width, point.x * cell + inset,
                  point.y * cell + inset, cell - inset * 2, cell - inset * 2,
                  i == 1 ? 0xB7F774 : 0x56C271);
        fill_rect(pixels, width, point.x * cell + inset + 2,
                  point.y * cell + inset + 2, cell - inset * 2 - 4,
                  cell / 8, i == 1 ? 0xE4FFB5 : 0x7BDD91);
    }
}

static uint64_t read_key_event(int keyboard_cap) {
    uint64_t payload[IPC_INLINE_WORDS] = {KEYBOARD_REQ_READ};
    ipc_msg_t reply = sys_ipc_call((uint32_t)keyboard_cap, 0, 8, payload);
    return reply.status < 0 ? 0 : reply.payload[0];
}

static int damage(int compositor_cap, uint32_t surface_id,
                  uint32_t width, uint32_t height) {
    uint64_t payload[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DAMAGE_SURFACE,
        surface_id,
        display_pack_pair(0, 0),
        display_pack_pair(width, height)
    };
    return sys_ipc_call((uint32_t)compositor_cap, 0, 32, payload).status;
}

void _start(void) {
    int compositor_cap = ns_resolve("compositor");
    int keyboard_cap = ns_resolve("keyboard");
    if (compositor_cap < 0 || keyboard_cap < 0) {
        printf("snake: graphical services unavailable\n");
        sys_exit(1);
    }

    uint64_t info_request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_INFO};
    ipc_msg_t info = sys_ipc_call((uint32_t)compositor_cap, 0, 8, info_request);
    if (info.status < 0) {
        printf("snake: graphical display unavailable\n");
        sys_exit(1);
    }

    uint32_t screen_width = (uint32_t)(info.payload[0] >> 32);
    uint32_t screen_height = (uint32_t)info.payload[0];
    uint32_t cell_x = (screen_width - screen_width / 10) / SNAKE_COLS;
    uint32_t cell_y = (screen_height - screen_height / 10) / SNAKE_ROWS;
    uint32_t cell = cell_x < cell_y ? cell_x : cell_y;
    if (cell > 40) cell = 40;
    uint32_t width = SNAKE_COLS * cell;
    uint32_t height = SNAKE_ROWS * cell;
    if (cell < 4 || screen_width < width || screen_height < height) {
        printf("snake: display is too small\n");
        sys_exit(1);
    }

    uint64_t size = ((uint64_t)width * height * sizeof(uint32_t) +
                     PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t *pixels = (uint32_t *)sys_mmap(size);
    if (!pixels) {
        printf("snake: surface allocation failed\n");
        sys_exit(1);
    }

    snake_point_t body[SNAKE_MAX];
    uint32_t length = 4;
    body[0] = (snake_point_t){12, 9};
    body[1] = (snake_point_t){11, 9};
    body[2] = (snake_point_t){10, 9};
    body[3] = (snake_point_t){9, 9};
    int dx = 1;
    int dy = 0;
    uint32_t score = 0;
    uint32_t seed = (uint32_t)sys_uptime_ns();
    snake_point_t food;
    place_food(&food, body, length, &seed);
    render(pixels, body, length, food, cell);

    int mem_cap = sys_mem_export(pixels, size, MEM_RIGHT_READ | MEM_RIGHT_WRITE |
                                               MEM_RIGHT_LEND);
    if (mem_cap < 0) {
        sys_munmap(pixels, size);
        printf("snake: surface export failed\n");
        sys_exit(1);
    }
    uint64_t create[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)mem_cap,
        [1] = 0,
        [2] = size,
        [3] = MEM_RIGHT_READ | IPC_MEM_MODE_LEND,
        [4] = COMPOSITOR_REQ_CREATE_SURFACE,
        [5] = display_pack_pair(width, height),
        [6] = display_pack_pair((screen_width - width) / 2,
                                (screen_height - height) / 2),
        [7] = width
    };
    ipc_msg_t created =
        sys_ipc_call((uint32_t)compositor_cap, IPC_FLAG_MEM, 64, create);
    if (created.status < 0 || created.payload[0] == 0) {
        sys_mem_revoke((uint32_t)mem_cap);
        sys_munmap(pixels, size);
        printf("snake: compositor rejected surface\n");
        sys_exit(1);
    }
    uint32_t surface_id = (uint32_t)created.payload[0];

    printf("snake: use WASD to move, q to quit\n");
    uint64_t next_step = sys_uptime_ms() + 180;
    int quit = 0;
    while (!quit) {
        while (sys_uptime_ms() < next_step) {
            uint64_t event = read_key_event(keyboard_cap);
            if (KEYBOARD_EVENT_TYPE(event) != KEYBOARD_EVENT_KEY ||
                (KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_PRESS &&
                 KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_REPEAT)) {
                continue;
            }
            char key = KEYBOARD_EVENT_CHAR(event);
            int next_dx = dx;
            int next_dy = dy;
            if (key == 'q' || key == 'Q') {
                quit = 1;
                break;
            }
            if (key == 'w' || key == 'W') {
                next_dx = 0; next_dy = -1;
            } else if (key == 's' || key == 'S') {
                next_dx = 0; next_dy = 1;
            } else if (key == 'a' || key == 'A') {
                next_dx = -1; next_dy = 0;
            } else if (key == 'd' || key == 'D') {
                next_dx = 1; next_dy = 0;
            }
            if (next_dx != -dx || next_dy != -dy) {
                dx = next_dx;
                dy = next_dy;
            }
        }
        if (quit) break;
        next_step += 180;

        int next_x = (int)body[0].x + dx;
        int next_y = (int)body[0].y + dy;
        if (next_x < 0 || next_x >= SNAKE_COLS ||
            next_y < 0 || next_y >= SNAKE_ROWS ||
            snake_contains(body, length - 1, (uint32_t)next_x,
                           (uint32_t)next_y)) {
            break;
        }

        int ate = food.x == (uint32_t)next_x && food.y == (uint32_t)next_y;
        if (ate && length < SNAKE_MAX) {
            length++;
            score++;
        }
        for (uint32_t i = length - 1; i > 0; i--) {
            body[i] = body[i - 1];
        }
        body[0] = (snake_point_t){(uint8_t)next_x, (uint8_t)next_y};
        if (ate) place_food(&food, body, length, &seed);
        render(pixels, body, length, food, cell);
        if (damage(compositor_cap, surface_id, width, height) < 0) break;
    }

    uint64_t destroy[IPC_INLINE_WORDS] = {
        COMPOSITOR_REQ_DESTROY_SURFACE, surface_id
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 16, destroy);
    (void)sys_mem_revoke((uint32_t)mem_cap);
    (void)sys_munmap(pixels, size);
    printf("snake: game over, score=%u\n", score);
    sys_exit(0);
}
