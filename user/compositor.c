#include "lib.h"
#include "ns_proto.h"
#include "compositor_proto.h"
#include "keyboard_proto.h"
#include "mouse_proto.h"

#define PAGE_SIZE 4096ULL
#define BACKGROUND_RGB 0x07111FU
#define CHROME_BORDER 3U
#define CHROME_TITLE_HEIGHT 28U
#define CHROME_ACTIVE 0x122033U
#define CHROME_INACTIVE 0x111827U
#define CHROME_BORDER_ACTIVE 0x82AFFFU
#define CHROME_BORDER_INACTIVE 0x374151U
#define CHROME_BUTTON 0xEF6F6CU
#define OWNER_SWEEP_MS 100ULL
#define PRESENT_INTERVAL_MS 8ULL
#define DIRTY_RECT_CAPACITY 16U
#define FULL_REPAINT_DIRTY_THRESHOLD 12U
#define FULL_REPAINT_AREA_PERCENT 85ULL
#define COMPOSITOR_EVENT_QUEUE_SIZE 32U
#define CURSOR_WIDTH 12U
#define CURSOR_HEIGHT 18U
#define MOUSE_POLL_INTERVAL_MS 2ULL

typedef struct {
    uint64_t type;
    uint64_t data0;
    uint64_t data1;
} compositor_event_t;

typedef struct {
    uint32_t present;
    uint32_t owner_tid;
    const uint32_t *pixels;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t x;
    uint32_t y;
    uint64_t flags;
    compositor_event_t events[COMPOSITOR_EVENT_QUEUE_SIZE];
    uint32_t event_head;
    uint32_t event_tail;
} surface_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} rect_t;

static surface_t surfaces[COMPOSITOR_MAX_SURFACES];
static rect_t dirty_rects[DIRTY_RECT_CAPACITY];
static uint32_t *backbuffer;
static uint64_t backbuffer_size;
static uint32_t screen_width;
static uint32_t screen_height;
static uint32_t display_buffer_id;
static int display_cap;
static display_present_queue_t *display_present_queue;
static int display_present_queue_cap = -1;
static int keyboard_cap;
static int mouse_cap = -1;
static mouse_ring_t *mouse_ring;
static int mouse_ring_cap = -1;
static int compositor_available;
static int scene_started;
static uint32_t focused_tid;
static uint32_t pointer_x;
static uint32_t pointer_y;
static uint32_t pointer_buttons;
static uint32_t pointer_max_x = 32767;
static uint32_t pointer_max_y = 32767;
static uint32_t dragging_surface_id;
static int32_t drag_offset_x;
static int32_t drag_offset_y;
static uint32_t dirty_count;
static uint64_t last_present_ms;
static uint64_t stat_damage_requests;
static uint64_t stat_dirty_rects_queued;
static uint64_t stat_dirty_rects_merged;
static uint64_t stat_dirty_queue_collapses;
static uint64_t stat_flushes;
static uint64_t stat_present_rects;
static uint64_t stat_present_queue_drops;
static uint64_t stat_max_dirty_rects;
static uint64_t stat_full_repaints;

static void queue_damage_rect(uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height);

static uint64_t round_pages(uint64_t size) {
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int overlap(uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
                   uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static void queue_compositor_event(surface_t *surface, uint64_t type,
                                   uint64_t data0, uint64_t data1) {
    uint32_t next = (surface->event_tail + 1) % COMPOSITOR_EVENT_QUEUE_SIZE;
    if (next != surface->event_head) {
        surface->events[surface->event_tail].type = type;
        surface->events[surface->event_tail].data0 = data0;
        surface->events[surface->event_tail].data1 = data1;
        surface->event_tail = next;
    }
}

static int dequeue_compositor_event(surface_t *surface,
                                    compositor_event_t *out) {
    if (surface->event_head == surface->event_tail) {
        return 0;
    }
    *out = surface->events[surface->event_head];
    surface->event_head =
        (surface->event_head + 1) % COMPOSITOR_EVENT_QUEUE_SIZE;
    return 1;
}

static int surface_has_chrome(const surface_t *surface) {
    return (surface->flags & COMPOSITOR_SURFACE_FLAG_CHROME) != 0;
}

static int surface_is_focusable(const surface_t *surface) {
    return (surface->flags & COMPOSITOR_SURFACE_FLAG_FOCUSABLE) != 0;
}

static void surface_outer_rect(const surface_t *surface, rect_t *rect) {
    uint32_t border = surface_has_chrome(surface) ? CHROME_BORDER : 0;
    uint32_t title = surface_has_chrome(surface) ? CHROME_TITLE_HEIGHT : 0;
    uint32_t top_margin = border + title;
    uint64_t right = (uint64_t)surface->x + surface->width + border;
    uint64_t bottom = (uint64_t)surface->y + surface->height + border;
    rect->x = surface->x > border ? surface->x - border : 0;
    rect->y = surface->y > top_margin ? surface->y - top_margin : 0;
    if (right > screen_width) {
        right = screen_width;
    }
    if (bottom > screen_height) {
        bottom = screen_height;
    }
    rect->width = right > rect->x ? (uint32_t)(right - rect->x) : 0;
    rect->height = bottom > rect->y ? (uint32_t)(bottom - rect->y) : 0;
}

static void queue_surface_damage(const surface_t *surface) {
    rect_t rect;
    surface_outer_rect(surface, &rect);
    queue_damage_rect(rect.x, rect.y, rect.width, rect.height);
}

static void fill_backbuffer_rect_clipped(uint32_t clip_x, uint32_t clip_y,
                                         uint32_t clip_w, uint32_t clip_h,
                                         uint32_t rect_x, uint32_t rect_y,
                                         uint32_t rect_w, uint32_t rect_h,
                                         uint32_t color) {
    if (!overlap(clip_x, clip_y, clip_w, clip_h,
                 rect_x, rect_y, rect_w, rect_h)) {
        return;
    }
    uint32_t left = clip_x > rect_x ? clip_x : rect_x;
    uint32_t top = clip_y > rect_y ? clip_y : rect_y;
    uint32_t right = clip_x + clip_w < rect_x + rect_w ?
                     clip_x + clip_w : rect_x + rect_w;
    uint32_t bottom = clip_y + clip_h < rect_y + rect_h ?
                      clip_y + clip_h : rect_y + rect_h;
    for (uint32_t row = top; row < bottom; row++) {
        uint32_t *dst = backbuffer + (uint64_t)row * screen_width + left;
        for (uint32_t col = left; col < right; col++) {
            *dst++ = color;
        }
    }
}

static void draw_surface_chrome(const surface_t *surface,
                                uint32_t clip_x, uint32_t clip_y,
                                uint32_t clip_w, uint32_t clip_h) {
    if (!surface_has_chrome(surface)) {
        return;
    }

    rect_t outer;
    surface_outer_rect(surface, &outer);
    uint32_t border = focused_tid == surface->owner_tid ?
                      CHROME_BORDER_ACTIVE : CHROME_BORDER_INACTIVE;
    uint32_t title = focused_tid == surface->owner_tid ?
                     CHROME_ACTIVE : CHROME_INACTIVE;

    fill_backbuffer_rect_clipped(clip_x, clip_y, clip_w, clip_h,
                                 outer.x, outer.y,
                                 outer.width, outer.height, border);
    fill_backbuffer_rect_clipped(clip_x, clip_y, clip_w, clip_h,
                                 surface->x, outer.y + CHROME_BORDER,
                                 surface->width, CHROME_TITLE_HEIGHT, title);
    fill_backbuffer_rect_clipped(clip_x, clip_y, clip_w, clip_h,
                                 surface->x + CHROME_BORDER,
                                 outer.y + CHROME_BORDER + 7U,
                                 12U, 12U, CHROME_BUTTON);
}

static void queue_cursor_damage_at(uint32_t x, uint32_t y) {
    queue_damage_rect(x, y, CURSOR_WIDTH, CURSOR_HEIGHT);
}

static void draw_cursor(uint32_t clip_x, uint32_t clip_y,
                        uint32_t clip_w, uint32_t clip_h) {
    static const uint16_t rows[CURSOR_HEIGHT] = {
        0x800, 0xC00, 0xE00, 0xF00, 0xF80, 0xFC0,
        0xFE0, 0xFF0, 0xF80, 0xD80, 0x8C0, 0x0C0,
        0x060, 0x060, 0x030, 0x030, 0x018, 0x018
    };
    if (!overlap(clip_x, clip_y, clip_w, clip_h,
                 pointer_x, pointer_y, CURSOR_WIDTH, CURSOR_HEIGHT)) {
        return;
    }
    for (uint32_t row = 0; row < CURSOR_HEIGHT; row++) {
        uint32_t y = pointer_y + row;
        if (y < clip_y || y >= clip_y + clip_h || y >= screen_height) {
            continue;
        }
        for (uint32_t col = 0; col < CURSOR_WIDTH; col++) {
            uint32_t x = pointer_x + col;
            if (x < clip_x || x >= clip_x + clip_w || x >= screen_width) {
                continue;
            }
            if ((rows[row] & (0x800U >> col)) == 0) {
                continue;
            }
            uint32_t edge =
                col == 0 || row == 0 ||
                (col + 1 < CURSOR_WIDTH &&
                 (rows[row] & (0x800U >> (col + 1))) == 0) ||
                (row + 1 < CURSOR_HEIGHT &&
                 (rows[row + 1] & (0x800U >> col)) == 0);
            backbuffer[(uint64_t)y * screen_width + x] =
                edge ? 0x000000 : 0xFFFFFF;
        }
    }
}

static void compose_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (x >= screen_width || y >= screen_height) {
        return;
    }
    if (width > screen_width - x) {
        width = screen_width - x;
    }
    if (height > screen_height - y) {
        height = screen_height - y;
    }

    for (uint32_t row = 0; row < height; row++) {
        uint32_t *dst = backbuffer + (uint64_t)(y + row) * screen_width + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = BACKGROUND_RGB;
        }
    }

    for (uint32_t id = 1; id < COMPOSITOR_MAX_SURFACES; id++) {
        surface_t *surface = &surfaces[id];
        if (!surface->present) {
            continue;
        }

        rect_t outer;
        surface_outer_rect(surface, &outer);
        if (!overlap(x, y, width, height, outer.x, outer.y,
                     outer.width, outer.height)) {
            continue;
        }
        draw_surface_chrome(surface, x, y, width, height);
        if (!overlap(x, y, width, height, surface->x, surface->y,
                     surface->width, surface->height)) {
            continue;
        }
        uint32_t left = x > surface->x ? x : surface->x;
        uint32_t top = y > surface->y ? y : surface->y;
        uint32_t right = x + width < surface->x + surface->width ?
                         x + width : surface->x + surface->width;
        uint32_t bottom = y + height < surface->y + surface->height ?
                          y + height : surface->y + surface->height;
        for (uint32_t row = top; row < bottom; row++) {
            const uint32_t *src =
                surface->pixels + (uint64_t)(row - surface->y) * surface->stride +
                (left - surface->x);
            uint32_t *dst = backbuffer + (uint64_t)row * screen_width + left;
            for (uint32_t col = left; col < right; col++) {
                *dst++ = *src++;
            }
        }
    }
    draw_cursor(x, y, width, height);
}

static int present_rect_ipc(uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height) {
    uint64_t payload[IPC_INLINE_WORDS] = {
        DISPLAY_REQ_BLIT_BUFFER,
        display_buffer_id,
        display_pack_pair(x, y),
        display_pack_pair(width, height)
    };
    ipc_msg_t reply = sys_ipc_call((uint32_t)display_cap, 0, 32, payload);
    return reply.status;
}

static int display_present_queue_ready(uint32_t *out_capacity) {
    if (display_present_queue &&
        display_present_queue->magic == DISPLAY_PRESENT_QUEUE_MAGIC &&
        display_present_queue->version == DISPLAY_PRESENT_QUEUE_VERSION) {
        uint32_t capacity = display_present_queue->capacity;
        if (capacity > 0 && capacity <= DISPLAY_PRESENT_QUEUE_CAPACITY) {
            *out_capacity = capacity;
            return 1;
        }
    }
    return 0;
}

static int present_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    uint32_t capacity = 0;
    if (display_present_queue_ready(&capacity)) {
        uint32_t write = display_present_queue->write_index % capacity;
        uint32_t next = (write + 1) % capacity;
        if (next != display_present_queue->read_index) {
            display_present_cmd_t *cmd =
                &display_present_queue->commands[write];
            cmd->buffer_id = display_buffer_id;
            cmd->x = x;
            cmd->y = y;
            cmd->width = width;
            cmd->height = height;
            cmd->reserved = 0;
            barrier();
            display_present_queue->write_index = next;
            return 0;
        }
        display_present_queue->dropped++;
        stat_present_queue_drops++;
    }

    return present_rect_ipc(x, y, width, height);
}

static uint32_t present_queue_free_slots(uint32_t read, uint32_t write,
                                         uint32_t capacity) {
    if (write >= read) {
        return capacity - (write - read) - 1U;
    }
    return read - write - 1U;
}

static int present_rects(const rect_t *rects, uint32_t count) {
    uint32_t capacity = 0;
    if (count == 0) {
        return 0;
    }
    if (display_present_queue_ready(&capacity)) {
        uint32_t read = display_present_queue->read_index % capacity;
        uint32_t write = display_present_queue->write_index % capacity;
        if (count <= present_queue_free_slots(read, write, capacity)) {
            uint32_t index = write;
            for (uint32_t i = 0; i < count; i++) {
                display_present_cmd_t *cmd =
                    &display_present_queue->commands[index];
                cmd->buffer_id = display_buffer_id;
                cmd->x = rects[i].x;
                cmd->y = rects[i].y;
                cmd->width = rects[i].width;
                cmd->height = rects[i].height;
                cmd->reserved = 0;
                index = (index + 1U) % capacity;
            }
            barrier();
            display_present_queue->write_index = index;
            return 0;
        }
        display_present_queue->dropped++;
        stat_present_queue_drops++;
    }

    int status = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (present_rect_ipc(rects[i].x, rects[i].y,
                             rects[i].width, rects[i].height) < 0) {
            status = -1;
        }
    }
    return status;
}

static int clamp_rect(rect_t *rect) {
    if (rect->width == 0 || rect->height == 0 ||
        rect->x >= screen_width || rect->y >= screen_height) {
        return 0;
    }
    if (rect->width > screen_width - rect->x) {
        rect->width = screen_width - rect->x;
    }
    if (rect->height > screen_height - rect->y) {
        rect->height = screen_height - rect->y;
    }
    return rect->width != 0 && rect->height != 0;
}

static int rects_touch_or_overlap(const rect_t *a, const rect_t *b) {
    uint32_t ar = a->x + a->width;
    uint32_t ab = a->y + a->height;
    uint32_t br = b->x + b->width;
    uint32_t bb = b->y + b->height;
    return a->x <= br && b->x <= ar && a->y <= bb && b->y <= ab;
}

static void merge_rect(rect_t *dst, const rect_t *src) {
    uint32_t left = dst->x < src->x ? dst->x : src->x;
    uint32_t top = dst->y < src->y ? dst->y : src->y;
    uint32_t dst_right = dst->x + dst->width;
    uint32_t src_right = src->x + src->width;
    uint32_t dst_bottom = dst->y + dst->height;
    uint32_t src_bottom = src->y + src->height;
    uint32_t right = dst_right > src_right ? dst_right : src_right;
    uint32_t bottom = dst_bottom > src_bottom ? dst_bottom : src_bottom;
    dst->x = left;
    dst->y = top;
    dst->width = right - left;
    dst->height = bottom - top;
}

static int should_full_repaint(void) {
    if (dirty_count >= FULL_REPAINT_DIRTY_THRESHOLD) {
        return 1;
    }

    uint64_t screen_area = (uint64_t)screen_width * screen_height;
    if (screen_area == 0) {
        return 0;
    }

    uint64_t dirty_area = 0;
    for (uint32_t i = 0; i < dirty_count; i++) {
        dirty_area += (uint64_t)dirty_rects[i].width * dirty_rects[i].height;
    }
    return dirty_area * 100ULL >= screen_area * FULL_REPAINT_AREA_PERCENT;
}

static void queue_damage_rect(uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height) {
    rect_t rect = {x, y, width, height};
    stat_damage_requests++;
    if (!scene_started) {
        scene_started = 1;
        rect.x = 0;
        rect.y = 0;
        rect.width = screen_width;
        rect.height = screen_height;
    }
    if (!clamp_rect(&rect)) {
        return;
    }

    for (uint32_t i = 0; i < dirty_count; i++) {
        if (rects_touch_or_overlap(&dirty_rects[i], &rect)) {
            merge_rect(&dirty_rects[i], &rect);
            stat_dirty_rects_merged++;
            return;
        }
    }

    if (dirty_count < DIRTY_RECT_CAPACITY) {
        dirty_rects[dirty_count++] = rect;
        stat_dirty_rects_queued++;
        if (dirty_count > stat_max_dirty_rects) {
            stat_max_dirty_rects = dirty_count;
        }
        return;
    }

    stat_dirty_queue_collapses++;
    merge_rect(&dirty_rects[0], &rect);
    for (uint32_t i = 1; i < dirty_count; i++) {
        merge_rect(&dirty_rects[0], &dirty_rects[i]);
    }
    dirty_count = 1;
}

static void flush_damage(int force) {
    if (dirty_count == 0) {
        return;
    }
    uint64_t now = sys_uptime_ms();
    if (!force && last_present_ms != 0 &&
        now - last_present_ms < PRESENT_INTERVAL_MS) {
        return;
    }

    if (should_full_repaint()) {
        compose_rect(0, 0, screen_width, screen_height);
        (void)present_rect(0, 0, screen_width, screen_height);
        stat_present_rects++;
        stat_flushes++;
        stat_full_repaints++;
        dirty_count = 0;
        last_present_ms = now;
        return;
    }

    for (uint32_t i = 0; i < dirty_count; i++) {
        rect_t *rect = &dirty_rects[i];
        compose_rect(rect->x, rect->y, rect->width, rect->height);
    }
    (void)present_rects(dirty_rects, dirty_count);
    stat_present_rects += dirty_count;
    stat_flushes++;
    dirty_count = 0;
    last_present_ms = now;
}

static void reset_stats(void) {
    stat_damage_requests = 0;
    stat_dirty_rects_queued = 0;
    stat_dirty_rects_merged = 0;
    stat_dirty_queue_collapses = 0;
    stat_flushes = 0;
    stat_present_rects = 0;
    stat_present_queue_drops = 0;
    stat_max_dirty_rects = dirty_count;
    stat_full_repaints = 0;
}

static int bind_display_present_queue(void) {
    if (display_present_queue) {
        return 0;
    }

    uint64_t queue_size = round_pages(sizeof(display_present_queue_t));
    display_present_queue = (display_present_queue_t *)sys_mmap(queue_size);
    if (!display_present_queue) {
        return -1;
    }

    uint8_t *bytes = (uint8_t *)display_present_queue;
    for (uint64_t i = 0; i < queue_size; i++) {
        bytes[i] = 0;
    }

    display_present_queue_cap =
        sys_mem_export(display_present_queue, queue_size,
                       MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE);
    if (display_present_queue_cap < 0) {
        display_present_queue = 0;
        return -1;
    }

    uint64_t payload[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)display_present_queue_cap,
        [1] = 0,
        [2] = queue_size,
        [3] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE,
        [4] = DISPLAY_REQ_BIND_PRESENT_QUEUE
    };
    ipc_msg_t bound =
        sys_ipc_call((uint32_t)display_cap, IPC_FLAG_MEM, 40, payload);
    if (bound.status < 0 ||
        display_present_queue->magic != DISPLAY_PRESENT_QUEUE_MAGIC ||
        display_present_queue->version != DISPLAY_PRESENT_QUEUE_VERSION) {
        display_present_queue = 0;
        return -1;
    }

    return 0;
}

static int initialize_display(void) {
    uint64_t info_request[IPC_INLINE_WORDS] = {DISPLAY_REQ_INFO};
    ipc_msg_t info = sys_ipc_call((uint32_t)display_cap, 0, 8, info_request);
    if (info.status < 0) {
        return -1;
    }
    uint64_t caps_request[IPC_INLINE_WORDS] = {DISPLAY_REQ_CAPS};
    ipc_msg_t caps = sys_ipc_call((uint32_t)display_cap, 0, 8, caps_request);
    if (caps.status < 0 ||
        (caps.payload[1] & DISPLAY_FEATURE_REGISTER_BUFFER) == 0 ||
        (caps.payload[1] & DISPLAY_FEATURE_BLIT_BUFFER) == 0) {
        return -1;
    }
    screen_width = (uint32_t)(info.payload[0] >> 32);
    screen_height = (uint32_t)info.payload[0];
    backbuffer_size = round_pages((uint64_t)screen_width * screen_height *
                                  sizeof(uint32_t));
    backbuffer = (uint32_t *)sys_mmap(backbuffer_size);
    if (!backbuffer) {
        return -1;
    }
    compose_rect(0, 0, screen_width, screen_height);

    int cap = sys_mem_export(backbuffer, backbuffer_size,
                             MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE);
    if (cap < 0) {
        return -1;
    }
    uint64_t payload[IPC_INLINE_WORDS] = {
        [0] = (uint64_t)cap,
        [1] = 0,
        [2] = backbuffer_size,
        [3] = MEM_RIGHT_READ | IPC_MEM_MODE_SHARE,
        [4] = DISPLAY_REQ_REGISTER_BUFFER,
        [5] = display_pack_pair(screen_width, screen_height),
        [6] = screen_width
    };
    ipc_msg_t registered =
        sys_ipc_call((uint32_t)display_cap, IPC_FLAG_MEM, 56, payload);
    if (registered.status < 0 || registered.payload[0] == 0) {
        return -1;
    }
    display_buffer_id = (uint32_t)registered.payload[0];
    if (caps.payload[1] & DISPLAY_FEATURE_PRESENT_QUEUE) {
        (void)bind_display_present_queue();
    }
    queue_damage_rect(0, 0, screen_width, screen_height);
    flush_damage(1);
    return 0;
}

static void set_keyboard_focus(uint32_t tid) {
    if (keyboard_cap < 0 || focused_tid == tid) {
        return;
    }
    uint32_t old_tid = focused_tid;
    uint64_t payload[IPC_INLINE_WORDS] = {KEYBOARD_REQ_SET_FOCUS, tid};
    ipc_msg_t reply = sys_ipc_call((uint32_t)keyboard_cap, 0, 16, payload);
    if (reply.status == 0) {
        focused_tid = tid;
        for (uint32_t id = 1; id < COMPOSITOR_MAX_SURFACES; id++) {
            if (!surfaces[id].present) {
                continue;
            }
            if (surfaces[id].owner_tid == old_tid) {
                queue_compositor_event(&surfaces[id],
                                       COMPOSITOR_EVENT_FOCUS, 0, 0);
                queue_surface_damage(&surfaces[id]);
            }
            if (surfaces[id].owner_tid == focused_tid) {
                queue_compositor_event(&surfaces[id],
                                       COMPOSITOR_EVENT_FOCUS, 1, 0);
                queue_surface_damage(&surfaces[id]);
            }
        }
    }
}

static void focus_topmost_surface(void) {
    for (uint32_t id = COMPOSITOR_MAX_SURFACES; id > 1; id--) {
        if (surfaces[id - 1].present && surface_is_focusable(&surfaces[id - 1])) {
            set_keyboard_focus(surfaces[id - 1].owner_tid);
            return;
        }
    }
    set_keyboard_focus(0);
}

static int point_in_content(const surface_t *surface, uint32_t x, uint32_t y) {
    return x >= surface->x && x < surface->x + surface->width &&
           y >= surface->y && y < surface->y + surface->height;
}

static int point_in_titlebar(const surface_t *surface, uint32_t x, uint32_t y) {
    if (!surface_has_chrome(surface)) {
        return 0;
    }
    rect_t outer;
    surface_outer_rect(surface, &outer);
    uint32_t title_y = outer.y + CHROME_BORDER;
    return x >= surface->x && x < surface->x + surface->width &&
           y >= title_y && y < title_y + CHROME_TITLE_HEIGHT;
}

static int point_in_close_button(const surface_t *surface,
                                 uint32_t x, uint32_t y) {
    if (!surface_has_chrome(surface)) {
        return 0;
    }
    rect_t outer;
    surface_outer_rect(surface, &outer);
    uint32_t bx = surface->x + CHROME_BORDER;
    uint32_t by = outer.y + CHROME_BORDER + 7U;
    return x >= bx && x < bx + 12U && y >= by && y < by + 12U;
}

static uint32_t topmost_surface_at(uint32_t x, uint32_t y) {
    for (uint32_t id = COMPOSITOR_MAX_SURFACES; id > 1; id--) {
        surface_t *surface = &surfaces[id - 1];
        if (!surface->present) {
            continue;
        }
        rect_t rect;
        surface_outer_rect(surface, &rect);
        if (x >= rect.x && x < rect.x + rect.width &&
            y >= rect.y && y < rect.y + rect.height) {
            return id - 1;
        }
    }
    return 0;
}

static void move_surface_internal(surface_t *surface,
                                  uint32_t x, uint32_t y) {
    uint32_t min_y = surface_has_chrome(surface) ?
                     CHROME_BORDER + CHROME_TITLE_HEIGHT : 0;
    uint32_t max_x = screen_width > surface->width ?
                     screen_width - surface->width : 0;
    uint32_t max_y = screen_height > surface->height ?
                     screen_height - surface->height : 0;
    x = min_u32(x, max_x);
    y = min_u32(max_u32(y, min_y), max_y);
    if (surface->x == x && surface->y == y) {
        return;
    }
    rect_t old_rect;
    surface_outer_rect(surface, &old_rect);
    surface->x = x;
    surface->y = y;
    queue_damage_rect(old_rect.x, old_rect.y,
                      old_rect.width, old_rect.height);
    queue_surface_damage(surface);
}

static uint32_t scale_pointer_axis(uint32_t value, uint32_t max,
                                   uint32_t extent) {
    if (extent <= 1 || max == 0) {
        return 0;
    }
    uint64_t scaled = (uint64_t)value * (extent - 1) / max;
    if (scaled >= extent) {
        scaled = extent - 1;
    }
    return (uint32_t)scaled;
}

static void queue_pointer_event_for_surface(surface_t *surface,
                                            uint64_t type,
                                            uint32_t screen_x,
                                            uint32_t screen_y,
                                            uint32_t buttons) {
    if (!point_in_content(surface, screen_x, screen_y)) {
        return;
    }
    uint32_t local_x = screen_x - surface->x;
    uint32_t local_y = screen_y - surface->y;
    queue_compositor_event(surface, type,
                           display_pack_pair(local_x, local_y),
                           buttons);
}

static void handle_pointer_event(uint32_t raw_x, uint32_t raw_y,
                                 uint32_t buttons) {
    uint32_t old_x = pointer_x;
    uint32_t old_y = pointer_y;
    uint32_t old_buttons = pointer_buttons;
    uint32_t new_x = scale_pointer_axis(raw_x, pointer_max_x, screen_width);
    uint32_t new_y = scale_pointer_axis(raw_y, pointer_max_y, screen_height);
    if (new_x != pointer_x || new_y != pointer_y) {
        queue_cursor_damage_at(pointer_x, pointer_y);
        pointer_x = new_x;
        pointer_y = new_y;
        queue_cursor_damage_at(pointer_x, pointer_y);
    }
    pointer_buttons = buttons;

    if (dragging_surface_id != 0) {
        surface_t *surface = &surfaces[dragging_surface_id];
        if (surface->present && (buttons & MOUSE_BUTTON_LEFT)) {
            int32_t new_x = (int32_t)pointer_x - drag_offset_x;
            int32_t new_y = (int32_t)pointer_y - drag_offset_y;
            uint32_t x = new_x > 0 ? (uint32_t)new_x : 0;
            uint32_t y = new_y > 0 ? (uint32_t)new_y : 0;
            move_surface_internal(surface, x, y);
            return;
        }
        dragging_surface_id = 0;
    }

    uint32_t hit_id = topmost_surface_at(pointer_x, pointer_y);
    surface_t *hit = hit_id ? &surfaces[hit_id] : 0;
    int left_down = (buttons & MOUSE_BUTTON_LEFT) != 0;
    int left_was_down = (old_buttons & MOUSE_BUTTON_LEFT) != 0;

    if (hit && !left_was_down && left_down) {
        if (surface_is_focusable(hit)) {
            set_keyboard_focus(hit->owner_tid);
        }
        if (point_in_close_button(hit, pointer_x, pointer_y)) {
            queue_compositor_event(hit, COMPOSITOR_EVENT_CLOSE, 0, 0);
            return;
        }
        if (point_in_titlebar(hit, pointer_x, pointer_y)) {
            dragging_surface_id = hit_id;
            drag_offset_x = (int32_t)pointer_x - (int32_t)hit->x;
            drag_offset_y = (int32_t)pointer_y - (int32_t)hit->y;
            return;
        }
        queue_pointer_event_for_surface(hit, COMPOSITOR_EVENT_POINTER_DOWN,
                                        pointer_x, pointer_y, buttons);
        return;
    }

    if (hit && left_was_down && !left_down) {
        queue_pointer_event_for_surface(hit, COMPOSITOR_EVENT_POINTER_UP,
                                        pointer_x, pointer_y, buttons);
        return;
    }

    if (hit && (old_x != pointer_x || old_y != pointer_y)) {
        queue_pointer_event_for_surface(hit, COMPOSITOR_EVENT_POINTER_MOVE,
                                        pointer_x, pointer_y, buttons);
    }
}

static int poll_mouse(void) {
    uint32_t before = dirty_count;
    if (mouse_cap < 0) {
        mouse_cap = ns_resolve("mouse");
        if (mouse_cap < 0) {
            return 0;
        }
        uint64_t info_request[IPC_INLINE_WORDS] = {MOUSE_REQ_INFO};
        ipc_msg_t info =
            sys_ipc_call((uint32_t)mouse_cap, 0, 8, info_request);
        if (info.status == 0) {
            pointer_max_x = (uint32_t)(info.payload[0] >> 32);
            pointer_max_y = (uint32_t)info.payload[0];
        }
    }

    if (!mouse_ring) {
        uint64_t ring_size = round_pages(sizeof(mouse_ring_t));
        mouse_ring = (mouse_ring_t *)sys_mmap(ring_size);
        if (!mouse_ring) {
            return 0;
        }
        uint8_t *bytes = (uint8_t *)mouse_ring;
        for (uint64_t i = 0; i < ring_size; i++) {
            bytes[i] = 0;
        }
        mouse_ring_cap = sys_mem_export(mouse_ring, ring_size,
                                        MEM_RIGHT_READ |
                                        MEM_RIGHT_WRITE |
                                        MEM_RIGHT_SHARE);
        if (mouse_ring_cap < 0) {
            mouse_ring = 0;
            return 0;
        }
        uint64_t bind[IPC_INLINE_WORDS] = {
            [0] = (uint64_t)mouse_ring_cap,
            [1] = 0,
            [2] = ring_size,
            [3] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE,
            [4] = MOUSE_REQ_BIND_RING
        };
        ipc_msg_t bound =
            sys_ipc_call((uint32_t)mouse_cap, IPC_FLAG_MEM, 40, bind);
        if (bound.status < 0 ||
            mouse_ring->magic != MOUSE_RING_MAGIC ||
            mouse_ring->version != MOUSE_RING_VERSION) {
            mouse_ring = 0;
            return 0;
        }
    }

    uint32_t capacity = mouse_ring->capacity;
    if (capacity == 0 || capacity > MOUSE_RING_CAPACITY) {
        return dirty_count != before;
    }
    for (uint32_t i = 0; i < 32 && mouse_ring->read_index != mouse_ring->write_index; i++) {
        uint32_t index = mouse_ring->read_index % capacity;
        mouse_ring_event_t event = mouse_ring->events[index];
        mouse_ring->read_index = (index + 1) % capacity;
        if (MOUSE_EVENT_TYPE(event.event) != MOUSE_EVENT_POINTER) {
            continue;
        }
        uint32_t buttons = (uint32_t)MOUSE_EVENT_BUTTONS(event.event);
        handle_pointer_event(event.x, event.y, buttons);
    }
    return dirty_count != before;
}

static int task_is_alive(uint32_t tid) {
    uint32_t capacity = sys_task_capacity();
    for (uint32_t index = 0; index < capacity; index++) {
        task_info_t info = sys_ps(index);
        if (info.present && info.tid == tid) {
            return 1;
        }
    }
    return 0;
}

static void clear_surface(surface_t *surface) {
    surface->present = 0;
    surface->owner_tid = 0;
    surface->pixels = 0;
    surface->size = 0;
    surface->width = 0;
    surface->height = 0;
    surface->stride = 0;
    surface->x = 0;
    surface->y = 0;
    surface->flags = 0;
    surface->event_head = 0;
    surface->event_tail = 0;
}

static void reap_dead_surfaces(void) {
    int removed = 0;
    for (uint32_t id = 1; id < COMPOSITOR_MAX_SURFACES; id++) {
        surface_t *surface = &surfaces[id];
        if (surface->present && !task_is_alive(surface->owner_tid)) {
            queue_surface_damage(surface);
            clear_surface(surface);
            removed = 1;
        }
    }
    if (removed) {
        focus_topmost_surface();
    }
}

static int create_surface(ipc_msg_t *msg) {
    uint32_t width = (uint32_t)(msg->payload[5] >> 32);
    uint32_t height = (uint32_t)msg->payload[5];
    uint32_t x = (uint32_t)(msg->payload[6] >> 32);
    uint32_t y = (uint32_t)msg->payload[6];
    uint32_t stride = (uint32_t)msg->payload[7];
    if (!msg->payload[0] || width == 0 || height == 0 || stride < width ||
        x >= screen_width || y >= screen_height ||
        width > screen_width - x || height > screen_height - y ||
        msg->payload[1] < (uint64_t)stride * height * sizeof(uint32_t)) {
        return -1;
    }
    for (uint32_t id = 1; id < COMPOSITOR_MAX_SURFACES; id++) {
        if (!surfaces[id].present) {
            surfaces[id].present = 1;
            surfaces[id].owner_tid = msg->sender_tid;
            surfaces[id].pixels = (const uint32_t *)msg->payload[0];
            surfaces[id].size = msg->payload[1];
            surfaces[id].width = width;
            surfaces[id].height = height;
            surfaces[id].stride = stride;
            surfaces[id].x = x;
            surfaces[id].y = y;
            surfaces[id].flags = COMPOSITOR_SURFACE_FLAG_FOCUSABLE;
            queue_surface_damage(&surfaces[id]);
            if (surface_is_focusable(&surfaces[id])) {
                set_keyboard_focus(msg->sender_tid);
            }
            return (int)id;
        }
    }
    return -1;
}

static surface_t *owned_surface(ipc_msg_t *msg, uint32_t id) {
    if (id == 0 || id >= COMPOSITOR_MAX_SURFACES ||
        !surfaces[id].present || surfaces[id].owner_tid != msg->sender_tid) {
        return 0;
    }
    return &surfaces[id];
}

void _start(void) {
    while ((display_cap = ns_resolve("display")) < 0) {
        sys_sleep(10);
    }
    while ((keyboard_cap = ns_resolve("keyboard")) < 0) {
        sys_sleep(10);
    }
    uint64_t bind[IPC_INLINE_WORDS] = {KEYBOARD_REQ_BIND_MANAGER};
    while (sys_ipc_call((uint32_t)keyboard_cap, 0, 8, bind).status < 0) {
        sys_sleep(10);
    }
    compositor_available = initialize_display() == 0;
    while (ns_register("compositor") < 0) {
        sys_sleep(10);
    }

    while (1) {
        uint64_t timeout_ms = dirty_count ? PRESENT_INTERVAL_MS :
                              MOUSE_POLL_INTERVAL_MS;
        ipc_msg_t msg = sys_ipc_recv_timeout(CAP_SELF, timeout_ms);
        int mouse_dirty = poll_mouse();
        if (mouse_dirty) {
            flush_damage(0);
        }
        if (msg.status < 0) {
            flush_damage(0);
            reap_dead_surfaces();
            flush_damage(0);
            continue;
        }
        reap_dead_surfaces();
        mouse_dirty = poll_mouse();
        if (mouse_dirty) {
            flush_damage(0);
        }
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
        int status = 0;
        uint64_t reply_len = 0;

        if (!compositor_available) {
            status = -1;
        } else if (msg.payload[0] == COMPOSITOR_REQ_INFO) {
            reply[0] = display_pack_pair(screen_width, screen_height);
            reply[1] = COMPOSITOR_MAX_SURFACES - 1;
            reply[2] = COMPOSITOR_ABI_VERSION;
            reply_len = 24;
        } else if (msg.payload[0] == COMPOSITOR_REQ_STATS) {
            reply[0] = stat_damage_requests;
            reply[1] = stat_dirty_rects_queued;
            reply[2] = stat_dirty_rects_merged;
            reply[3] = stat_dirty_queue_collapses;
            reply[4] = stat_flushes;
            reply[5] = stat_present_rects;
            reply[6] = stat_max_dirty_rects;
            reply[7] = stat_full_repaints;
            reply[8] = stat_present_queue_drops;
            reply_len = 72;
        } else if (msg.payload[0] == COMPOSITOR_REQ_RESET_STATS) {
            reset_stats();
        } else if (msg.payload[4] == COMPOSITOR_REQ_CREATE_SURFACE) {
            int id = create_surface(&msg);
            status = id < 0 ? -1 : 0;
            reply[0] = id < 0 ? 0 : (uint64_t)id;
            reply_len = 8;
        } else if (msg.payload[0] == COMPOSITOR_REQ_DAMAGE_SURFACE) {
            surface_t *surface = owned_surface(&msg, (uint32_t)msg.payload[1]);
            if (!surface) {
                status = -1;
            } else {
                uint32_t x = (uint32_t)(msg.payload[2] >> 32);
                uint32_t y = (uint32_t)msg.payload[2];
                uint32_t width = (uint32_t)(msg.payload[3] >> 32);
                uint32_t height = (uint32_t)msg.payload[3];
                if (x >= surface->width || y >= surface->height) {
                    status = -1;
                } else {
                    if (width > surface->width - x) width = surface->width - x;
                    if (height > surface->height - y) height = surface->height - y;
                    queue_damage_rect(surface->x + x, surface->y + y, width, height);
                }
            }
        } else if (msg.payload[0] == COMPOSITOR_REQ_SET_SURFACE_FLAGS) {
            surface_t *surface = owned_surface(&msg, (uint32_t)msg.payload[1]);
            if (!surface) {
                status = -1;
            } else {
                uint64_t flags = msg.payload[2] &
                    (COMPOSITOR_SURFACE_FLAG_FOCUSABLE |
                     COMPOSITOR_SURFACE_FLAG_CHROME |
                     COMPOSITOR_SURFACE_FLAG_RESIZABLE);
                queue_surface_damage(surface);
                surface->flags = flags;
                queue_surface_damage(surface);
                if (!surface_is_focusable(surface) &&
                    focused_tid == surface->owner_tid) {
                    focus_topmost_surface();
                } else if (surface_is_focusable(surface)) {
                    set_keyboard_focus(surface->owner_tid);
                }
            }
        } else if (msg.payload[0] == COMPOSITOR_REQ_MOVE_SURFACE) {
            surface_t *surface = owned_surface(&msg, (uint32_t)msg.payload[1]);
            if (!surface) {
                status = -1;
            } else {
                uint32_t x = (uint32_t)(msg.payload[2] >> 32);
                uint32_t y = (uint32_t)msg.payload[2];
                if (x >= screen_width || y >= screen_height ||
                    surface->width > screen_width - x ||
                    surface->height > screen_height - y) {
                    status = -1;
                } else {
                    move_surface_internal(surface, x, y);
                }
            }
        } else if (msg.payload[0] == COMPOSITOR_REQ_DESTROY_SURFACE) {
            surface_t *surface = owned_surface(&msg, (uint32_t)msg.payload[1]);
            if (!surface) {
                status = -1;
            } else {
                rect_t old_rect;
                surface_outer_rect(surface, &old_rect);
                uint32_t owner_tid = surface->owner_tid;
                clear_surface(surface);
                queue_damage_rect(old_rect.x, old_rect.y,
                                  old_rect.width, old_rect.height);
                if (focused_tid == owner_tid) {
                    focus_topmost_surface();
                }
            }
        } else if (msg.payload[0] == COMPOSITOR_REQ_POLL_EVENT) {
            surface_t *surface = owned_surface(&msg, (uint32_t)msg.payload[1]);
            if (!surface) {
                status = -1;
            } else {
                compositor_event_t event = {0, 0, 0};
                (void)dequeue_compositor_event(surface, &event);
                reply[0] = event.type;
                reply[1] = event.data0;
                reply[2] = event.data1;
                reply_len = 24;
            }
        } else {
            status = -1;
        }

        sys_ipc_reply(msg.reply_cap, status, 0, reply_len, reply);
        flush_damage(0);
    }
}
