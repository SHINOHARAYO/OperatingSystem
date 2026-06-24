#include "lib.h"
#include "ns_proto.h"
#include "display_proto.h"

static display_boot_info_t *display;
static volatile uint32_t *framebuffer;
static int display_available;
static uint32_t screen_width;
static uint32_t screen_height;
static uint32_t screen_stride;
static uint32_t screen_format;
static uint64_t screen_bytes;

typedef struct {
    uint32_t present;
    const uint32_t *pixels;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} display_buffer_t;

typedef struct {
    uint64_t type;
    uint64_t features;
    void (*fill_rect)(uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint32_t rgb);
    int (*register_buffer)(const uint32_t *pixels, uint64_t size,
                           uint32_t width, uint32_t height, uint32_t stride);
    int (*blit_buffer)(uint32_t id, uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height);
} display_backend_t;

static display_buffer_t buffers[DISPLAY_MAX_BUFFERS];
static const display_backend_t *backend;
static display_present_queue_t *present_queue;
static uint64_t stat_fill_rects;
static uint64_t stat_registered_buffers;
static uint64_t stat_blit_rects;
static uint64_t stat_present_queue_commands;
static uint64_t stat_errors;
static uint64_t stat_gpu_commands;
static uint64_t stat_gpu_transfers;
static uint64_t stat_gpu_flushes;
static uint64_t stat_gpu_transfer_bytes;
static uint64_t stat_gpu_queued_rects;
static uint64_t stat_gpu_merged_rects;
static uint64_t stat_gpu_batch_flushes;
static uint64_t stat_gpu_max_dirty_rects;

#define VIRTIO_PCI_COMMON_DFSELECT       0
#define VIRTIO_PCI_COMMON_DF             4
#define VIRTIO_PCI_COMMON_GFSELECT       8
#define VIRTIO_PCI_COMMON_GF             12
#define VIRTIO_PCI_COMMON_STATUS         20
#define VIRTIO_PCI_COMMON_Q_SELECT       22
#define VIRTIO_PCI_COMMON_Q_SIZE         24
#define VIRTIO_PCI_COMMON_Q_ENABLE       28
#define VIRTIO_PCI_COMMON_Q_NOTIFY_OFF   30
#define VIRTIO_PCI_COMMON_Q_DESC         32
#define VIRTIO_PCI_COMMON_Q_DRIVER       40
#define VIRTIO_PCI_COMMON_Q_DEVICE       48

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128
#define VIRTIO_F_VERSION_1_LOW_BIT 0

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_SIZE 32

#define VIRTIO_GPU_RESOURCE_ID 1U
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2U
#define VIRTIO_GPU_MAX_SCANOUTS 16U
#define GPU_REQUEST_PAGES 16U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101U
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101U
#define DISPLAY_PRESENT_QUEUE_POLL_MS 2ULL
#define GPU_PRESENT_INTERVAL_MS 4ULL
#define GPU_DIRTY_RECT_CAPACITY 32U
#define GPU_FULL_REPAINT_DIRTY_THRESHOLD 24U
#define GPU_FULL_REPAINT_AREA_PERCENT 85ULL

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
    uint16_t used_event;
} virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTQ_SIZE];
    uint16_t avail_event;
} virtq_used_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} virtio_gpu_rect_t;

typedef struct {
    virtio_gpu_rect_t rect;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} virtio_gpu_resource_attach_backing_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t rect;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

static volatile uint8_t *gpu_bar;
static volatile uint8_t *gpu_common;
static volatile uint8_t *gpu_notify;
static volatile uint8_t *gpu_notify_base;
static volatile uint8_t *gpu_isr;
static uint32_t gpu_notify_multiplier;
static virtq_desc_t *gpu_vq_desc;
static virtq_avail_t *gpu_vq_avail;
static virtq_used_t *gpu_vq_used;
static uint64_t gpu_vq_desc_pa;
static uint64_t gpu_vq_avail_pa;
static uint64_t gpu_vq_used_pa;
static uint16_t gpu_vq_avail_idx;
static uint16_t gpu_vq_used_idx;
static unsigned char *gpu_request;
static unsigned char *gpu_response;
static uint64_t gpu_request_pa;
static uint64_t gpu_response_pa;
static uint32_t *gpu_framebuffer;
static uint64_t gpu_framebuffer_size;
static uint32_t gpu_framebuffer_dma_cap;
static uint32_t gpu_width;
static uint32_t gpu_height;
static int gpu_init_error;
static virtio_gpu_rect_t gpu_dirty_rects[GPU_DIRTY_RECT_CAPACITY];
static uint32_t gpu_dirty_count;
static uint64_t gpu_last_present_ms;

static uint32_t mask_shift(uint32_t mask) {
    uint32_t shift = 0;
    if (!mask) {
        return 0;
    }
    while ((mask & 1U) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static void zero_bytes(void *ptr, uint64_t size) {
    unsigned char *p = (unsigned char *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static uint64_t round_pages(uint64_t size) {
    return (size + 4095ULL) & ~4095ULL;
}

static uint8_t pci_read8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static uint16_t pci_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static uint32_t pci_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void pci_write8(volatile uint8_t *base, uint32_t off, uint8_t value) {
    *(volatile uint8_t *)(base + off) = value;
}

static void pci_write16(volatile uint8_t *base, uint32_t off, uint16_t value) {
    *(volatile uint16_t *)(base + off) = value;
}

static void pci_write32(volatile uint8_t *base, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(base + off) = value;
}

static void pci_write64(volatile uint8_t *base, uint32_t off, uint64_t value) {
    *(volatile uint64_t *)(base + off) = value;
}

static int dma_export_range(void *addr, uint64_t size, uint64_t *out_pa,
                            uint32_t *out_cap) {
    int cap = sys_dma_export(addr, size, OCAP_RIGHT_READ | OCAP_RIGHT_WRITE);
    if (cap < 0) {
        return -1;
    }
    uint64_t pa = sys_dma_paddr((uint32_t)cap, 0);
    if (!pa) {
        sys_dma_release((uint32_t)cap);
        return -1;
    }
    if (out_pa) {
        *out_pa = pa;
    }
    if (out_cap) {
        *out_cap = (uint32_t)cap;
    }
    return 0;
}

static void *dma_page(uint64_t *out_pa) {
    unsigned char *page = (unsigned char *)sys_mmap(4096);
    if (!page) {
        return 0;
    }
    if (dma_export_range(page, 4096, out_pa, 0) < 0) {
        sys_munmap(page, 4096);
        return 0;
    }
    zero_bytes(page, 4096);
    return page;
}

static void *dma_pages(uint32_t page_count, uint64_t *out_pa) {
    unsigned char *mem = (unsigned char *)sys_mmap((uint64_t)page_count * 4096);
    if (!mem) {
        return 0;
    }
    uint64_t base_pa = 0;
    uint32_t cap = 0;
    if (dma_export_range(mem, (uint64_t)page_count * 4096, &base_pa, &cap) < 0) {
        sys_munmap(mem, (uint64_t)page_count * 4096);
        return 0;
    }
    for (uint32_t i = 0; i < page_count; i++) {
        uint64_t pa = sys_dma_paddr(cap, (uint64_t)i * 4096);
        if (!pa || pa != base_pa + ((uint64_t)i * 4096)) {
            sys_dma_release(cap);
            sys_munmap(mem, (uint64_t)page_count * 4096);
            return 0;
        }
    }
    zero_bytes(mem, (uint64_t)page_count * 4096);
    *out_pa = base_pa;
    return mem;
}

static int gpu_setup_queue(void) {
    pci_write16(gpu_common, VIRTIO_PCI_COMMON_Q_SELECT, 0);
    uint16_t qmax = pci_read16(gpu_common, VIRTIO_PCI_COMMON_Q_SIZE);
    if (qmax < VIRTQ_SIZE) {
        return -1;
    }

    gpu_vq_desc = (virtq_desc_t *)dma_page(&gpu_vq_desc_pa);
    gpu_vq_avail = (virtq_avail_t *)dma_page(&gpu_vq_avail_pa);
    gpu_vq_used = (virtq_used_t *)dma_page(&gpu_vq_used_pa);
    gpu_request = (unsigned char *)dma_pages(GPU_REQUEST_PAGES, &gpu_request_pa);
    gpu_response = (unsigned char *)dma_page(&gpu_response_pa);
    if (!gpu_vq_desc || !gpu_vq_avail || !gpu_vq_used ||
        !gpu_request || !gpu_response) {
        return -1;
    }

    pci_write16(gpu_common, VIRTIO_PCI_COMMON_Q_SIZE, VIRTQ_SIZE);
    uint16_t notify_off = pci_read16(gpu_common, VIRTIO_PCI_COMMON_Q_NOTIFY_OFF);
    gpu_notify = gpu_notify_base + ((uint64_t)notify_off * gpu_notify_multiplier);
    pci_write64(gpu_common, VIRTIO_PCI_COMMON_Q_DESC, gpu_vq_desc_pa);
    pci_write64(gpu_common, VIRTIO_PCI_COMMON_Q_DRIVER, gpu_vq_avail_pa);
    pci_write64(gpu_common, VIRTIO_PCI_COMMON_Q_DEVICE, gpu_vq_used_pa);
    pci_write16(gpu_common, VIRTIO_PCI_COMMON_Q_ENABLE, 1);
    return 0;
}

static int gpu_command(void *request, uint32_t request_len,
                       void *response, uint32_t response_len,
                       uint32_t expected_response) {
    if (!request || !response || request_len == 0 ||
        request_len > GPU_REQUEST_PAGES * 4096 || response_len > 4096) {
        return -1;
    }

    zero_bytes(gpu_response, 4096);
    if (request != gpu_request) {
        zero_bytes(gpu_request, GPU_REQUEST_PAGES * 4096);
        unsigned char *dst = gpu_request;
        unsigned char *src = (unsigned char *)request;
        for (uint32_t i = 0; i < request_len; i++) {
            dst[i] = src[i];
        }
    }

    gpu_vq_desc[0].addr = gpu_request_pa;
    gpu_vq_desc[0].len = request_len;
    gpu_vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    gpu_vq_desc[0].next = 1;
    gpu_vq_desc[1].addr = gpu_response_pa;
    gpu_vq_desc[1].len = response_len;
    gpu_vq_desc[1].flags = VIRTQ_DESC_F_WRITE;
    gpu_vq_desc[1].next = 0;

    gpu_vq_avail->ring[gpu_vq_avail_idx % VIRTQ_SIZE] = 0;
    barrier();
    gpu_vq_avail_idx++;
    gpu_vq_avail->idx = gpu_vq_avail_idx;
    barrier();
    pci_write16(gpu_notify, 0, 0);
    stat_gpu_commands++;

    for (uint64_t spins = 0; spins < 10000000; spins++) {
        barrier();
        if (gpu_vq_used->idx != gpu_vq_used_idx) {
            gpu_vq_used_idx = gpu_vq_used->idx;
            (void)pci_read8(gpu_isr, 0);
            uint32_t type = ((virtio_gpu_ctrl_hdr_t *)gpu_response)->type;
            if (type != expected_response) {
                stat_errors++;
                return -1;
            }
            unsigned char *resp_dst = (unsigned char *)response;
            unsigned char *resp_src = gpu_response;
            for (uint32_t i = 0; i < response_len; i++) {
                resp_dst[i] = resp_src[i];
            }
            return 0;
        }
    }
    return -1;
}

static int gpu_rects_touch_or_overlap(const virtio_gpu_rect_t *a,
                                      const virtio_gpu_rect_t *b) {
    uint32_t ar = a->x + a->width;
    uint32_t ab = a->y + a->height;
    uint32_t br = b->x + b->width;
    uint32_t bb = b->y + b->height;
    return a->x <= br && b->x <= ar && a->y <= bb && b->y <= ab;
}

static void gpu_merge_rect(virtio_gpu_rect_t *dst,
                           const virtio_gpu_rect_t *src) {
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

static int gpu_clamp_rect(virtio_gpu_rect_t *rect) {
    if (rect->width == 0 || rect->height == 0 ||
        rect->x >= gpu_width || rect->y >= gpu_height) {
        return 0;
    }
    if (rect->width > gpu_width - rect->x) rect->width = gpu_width - rect->x;
    if (rect->height > gpu_height - rect->y) rect->height = gpu_height - rect->y;
    return rect->width != 0 && rect->height != 0;
}

static void gpu_queue_present_rect(uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height) {
    virtio_gpu_rect_t rect = {x, y, width, height};
    if (!gpu_clamp_rect(&rect)) {
        return;
    }

    for (uint32_t i = 0; i < gpu_dirty_count; i++) {
        if (gpu_rects_touch_or_overlap(&gpu_dirty_rects[i], &rect)) {
            gpu_merge_rect(&gpu_dirty_rects[i], &rect);
            stat_gpu_merged_rects++;
            return;
        }
    }

    if (gpu_dirty_count < GPU_DIRTY_RECT_CAPACITY) {
        gpu_dirty_rects[gpu_dirty_count++] = rect;
        stat_gpu_queued_rects++;
        if (gpu_dirty_count > stat_gpu_max_dirty_rects) {
            stat_gpu_max_dirty_rects = gpu_dirty_count;
        }
        return;
    }

    gpu_merge_rect(&gpu_dirty_rects[0], &rect);
    for (uint32_t i = 1; i < gpu_dirty_count; i++) {
        gpu_merge_rect(&gpu_dirty_rects[0], &gpu_dirty_rects[i]);
    }
    gpu_dirty_count = 1;
}

static int gpu_should_full_repaint(void) {
    if (gpu_dirty_count >= GPU_FULL_REPAINT_DIRTY_THRESHOLD) {
        return 1;
    }
    uint64_t screen_area = (uint64_t)gpu_width * gpu_height;
    uint64_t dirty_area = 0;
    if (!screen_area) {
        return 0;
    }
    for (uint32_t i = 0; i < gpu_dirty_count; i++) {
        dirty_area += (uint64_t)gpu_dirty_rects[i].width *
                      gpu_dirty_rects[i].height;
    }
    return dirty_area * 100ULL >= screen_area * GPU_FULL_REPAINT_AREA_PERCENT;
}

static uint32_t mask_width(uint32_t mask) {
    uint32_t width = 0;
    while (mask) {
        width += mask & 1U;
        mask >>= 1;
    }
    return width;
}

static uint32_t channel_to_mask(uint32_t channel, uint32_t mask) {
    uint32_t width = mask_width(mask);
    if (!mask || width == 0) {
        return 0;
    }
    uint32_t max = width >= 32 ? 0xFFFFFFFFU : ((1U << width) - 1U);
    uint32_t scaled = (channel * max + 127U) / 255U;
    return (scaled << mask_shift(mask)) & mask;
}

static uint32_t native_color(uint32_t rgb) {
    uint32_t red = (rgb >> 16) & 0xFFU;
    uint32_t green = (rgb >> 8) & 0xFFU;
    uint32_t blue = rgb & 0xFFU;

    if (display->pixel_format == DISPLAY_PIXEL_RGB_RESERVED_8BIT) {
        return red | (green << 8) | (blue << 16);
    }
    if (display->pixel_format == DISPLAY_PIXEL_BGR_RESERVED_8BIT) {
        return blue | (green << 8) | (red << 16);
    }
    return channel_to_mask(red, display->red_mask) |
           channel_to_mask(green, display->green_mask) |
           channel_to_mask(blue, display->blue_mask);
}

static void ramfb_fill_rect(uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, uint32_t rgb) {
    if (x >= display->width || y >= display->height) {
        return;
    }
    if (width > display->width - x) {
        width = display->width - x;
    }
    if (height > display->height - y) {
        height = display->height - y;
    }

    uint32_t color = native_color(rgb);
    for (uint32_t row = 0; row < height; row++) {
        volatile uint32_t *line =
            framebuffer + (uint64_t)(y + row) * display->pixels_per_scan_line + x;
        for (uint32_t col = 0; col < width; col++) {
            line[col] = color;
        }
    }
}

static int ramfb_register_buffer(const uint32_t *pixels, uint64_t size,
                                 uint32_t width, uint32_t height,
                                 uint32_t stride) {
    if (!pixels || width == 0 || height == 0 || stride < width ||
        size < (uint64_t)stride * height * sizeof(uint32_t)) {
        return -1;
    }
    for (uint32_t i = 1; i < DISPLAY_MAX_BUFFERS; i++) {
        if (!buffers[i].present) {
            buffers[i].present = 1;
            buffers[i].pixels = pixels;
            buffers[i].size = size;
            buffers[i].width = width;
            buffers[i].height = height;
            buffers[i].stride = stride;
            return (int)i;
        }
    }
    return -1;
}

static int ramfb_blit_buffer(uint32_t id, uint32_t x, uint32_t y,
                             uint32_t width, uint32_t height) {
    if (id == 0 || id >= DISPLAY_MAX_BUFFERS || !buffers[id].present) {
        return -1;
    }
    display_buffer_t *buffer = &buffers[id];
    if (x >= buffer->width || y >= buffer->height ||
        x >= display->width || y >= display->height) {
        return 0;
    }
    if (width > buffer->width - x) {
        width = buffer->width - x;
    }
    if (height > buffer->height - y) {
        height = buffer->height - y;
    }
    if (width > display->width - x) {
        width = display->width - x;
    }
    if (height > display->height - y) {
        height = display->height - y;
    }

    for (uint32_t row = 0; row < height; row++) {
        const uint32_t *src =
            buffer->pixels + (uint64_t)(y + row) * buffer->stride + x;
        volatile uint32_t *dst =
            framebuffer + (uint64_t)(y + row) * display->pixels_per_scan_line + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = native_color(src[col]);
        }
    }
    return 0;
}

static int gpu_transfer_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (x >= gpu_width || y >= gpu_height) {
        return 0;
    }
    if (width > gpu_width - x) width = gpu_width - x;
    if (height > gpu_height - y) height = gpu_height - y;
    if (width == 0 || height == 0) {
        return 0;
    }

    virtio_gpu_ctrl_hdr_t resp;
    virtio_gpu_transfer_to_host_2d_t transfer = {
        .hdr = {.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D},
        .rect = {x, y, width, height},
        .offset = ((uint64_t)y * gpu_width + x) * sizeof(uint32_t),
        .resource_id = VIRTIO_GPU_RESOURCE_ID
    };
    if (gpu_command(&transfer, sizeof(transfer), &resp, sizeof(resp),
                    VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        return -1;
    }
    stat_gpu_transfers++;
    stat_gpu_transfer_bytes += (uint64_t)width * height * sizeof(uint32_t);
    return 0;
}

static int gpu_flush_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    virtio_gpu_ctrl_hdr_t resp;
    virtio_gpu_resource_flush_t flush = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH},
        .rect = {x, y, width, height},
        .resource_id = VIRTIO_GPU_RESOURCE_ID
    };
    if (gpu_command(&flush, sizeof(flush), &resp, sizeof(resp),
                    VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        return -1;
    }
    stat_gpu_flushes++;
    return 0;
}

static int gpu_present_rect_now(uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
    if (gpu_transfer_rect(x, y, width, height) < 0) {
        return -1;
    }
    return gpu_flush_rect(x, y, width, height);
}

static int gpu_flush_queued(int force) {
    if (gpu_dirty_count == 0) {
        return 0;
    }
    uint64_t now = sys_uptime_ms();
    if (!force && gpu_last_present_ms != 0 &&
        now - gpu_last_present_ms < GPU_PRESENT_INTERVAL_MS) {
        return 0;
    }

    int status = 0;
    if (gpu_should_full_repaint()) {
        status = gpu_present_rect_now(0, 0, gpu_width, gpu_height);
    } else {
        for (uint32_t i = 0; i < gpu_dirty_count; i++) {
            virtio_gpu_rect_t *rect = &gpu_dirty_rects[i];
            if (gpu_transfer_rect(rect->x, rect->y,
                                  rect->width, rect->height) < 0) {
                status = -1;
                break;
            }
        }
        if (status == 0) {
            for (uint32_t i = 0; i < gpu_dirty_count; i++) {
                virtio_gpu_rect_t *rect = &gpu_dirty_rects[i];
                if (gpu_flush_rect(rect->x, rect->y,
                                   rect->width, rect->height) < 0) {
                    status = -1;
                    break;
                }
            }
        }
    }

    stat_gpu_batch_flushes++;
    gpu_dirty_count = 0;
    gpu_last_present_ms = now;
    if (status < 0) {
        stat_errors++;
    }
    return status;
}

static void gpu_fill_rect(uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height, uint32_t rgb) {
    if (x >= gpu_width || y >= gpu_height) {
        return;
    }
    if (width > gpu_width - x) width = gpu_width - x;
    if (height > gpu_height - y) height = gpu_height - y;

    for (uint32_t row = 0; row < height; row++) {
        uint32_t *line = gpu_framebuffer + (uint64_t)(y + row) * gpu_width + x;
        for (uint32_t col = 0; col < width; col++) {
            line[col] = rgb;
        }
    }
    gpu_queue_present_rect(x, y, width, height);
}

static int gpu_register_buffer(const uint32_t *pixels, uint64_t size,
                               uint32_t width, uint32_t height,
                               uint32_t stride) {
    return ramfb_register_buffer(pixels, size, width, height, stride);
}

static int gpu_blit_buffer(uint32_t id, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) {
    if (id == 0 || id >= DISPLAY_MAX_BUFFERS || !buffers[id].present) {
        return -1;
    }
    display_buffer_t *buffer = &buffers[id];
    if (x >= buffer->width || y >= buffer->height ||
        x >= gpu_width || y >= gpu_height) {
        return 0;
    }
    if (width > buffer->width - x) width = buffer->width - x;
    if (height > buffer->height - y) height = buffer->height - y;
    if (width > gpu_width - x) width = gpu_width - x;
    if (height > gpu_height - y) height = gpu_height - y;

    for (uint32_t row = 0; row < height; row++) {
        const uint32_t *src =
            buffer->pixels + (uint64_t)(y + row) * buffer->stride + x;
        uint32_t *dst = gpu_framebuffer + (uint64_t)(y + row) * gpu_width + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = src[col];
        }
    }
    gpu_queue_present_rect(x, y, width, height);
    return 0;
}

static int virtio_gpu_init(void) {
    gpu_init_error = 0;
    if (display->gpu_present == 0 ||
        display->gpu_transport != DISPLAY_GPU_TRANSPORT_PCI_MODERN ||
        display->gpu_bar_va == 0 || display->gpu_bar_size == 0) {
        gpu_init_error = 1;
        return -1;
    }

    gpu_bar = (volatile uint8_t *)display->gpu_bar_va;
    gpu_common = gpu_bar + display->gpu_common_off;
    gpu_notify_base = gpu_bar + display->gpu_notify_off;
    gpu_isr = gpu_bar + display->gpu_isr_off;
    gpu_notify_multiplier = display->gpu_notify_multiplier;

    pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, 0);
    barrier();
    pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    pci_write32(gpu_common, VIRTIO_PCI_COMMON_DFSELECT, 1);
    uint32_t features_hi = pci_read32(gpu_common, VIRTIO_PCI_COMMON_DF);
    if (!(features_hi & (1U << VIRTIO_F_VERSION_1_LOW_BIT))) {
        pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        gpu_init_error = 2;
        return -1;
    }
    pci_write32(gpu_common, VIRTIO_PCI_COMMON_GFSELECT, 0);
    pci_write32(gpu_common, VIRTIO_PCI_COMMON_GF, 0);
    pci_write32(gpu_common, VIRTIO_PCI_COMMON_GFSELECT, 1);
    pci_write32(gpu_common, VIRTIO_PCI_COMMON_GF,
                1U << VIRTIO_F_VERSION_1_LOW_BIT);

    status |= VIRTIO_STATUS_FEATURES_OK;
    pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, status);
    if (!(pci_read8(gpu_common, VIRTIO_PCI_COMMON_STATUS) &
          VIRTIO_STATUS_FEATURES_OK)) {
        pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        gpu_init_error = 3;
        return -1;
    }

    if (gpu_setup_queue() < 0) {
        pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        gpu_init_error = 4;
        return -1;
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    pci_write8(gpu_common, VIRTIO_PCI_COMMON_STATUS, status);
    barrier();

    virtio_gpu_ctrl_hdr_t get_info = {
        .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO
    };
    virtio_gpu_resp_display_info_t info;
    if (gpu_command(&get_info, sizeof(get_info), &info, sizeof(info),
                    VIRTIO_GPU_RESP_OK_DISPLAY_INFO) < 0) {
        gpu_init_error = 5;
        return -1;
    }

    gpu_width = info.pmodes[0].rect.width;
    gpu_height = info.pmodes[0].rect.height;
    if (gpu_width == 0 || gpu_height == 0) {
        gpu_width = display->width ? display->width : 1024;
        gpu_height = display->height ? display->height : 768;
    }

    gpu_framebuffer_size = round_pages((uint64_t)gpu_width * gpu_height *
                                       sizeof(uint32_t));
    uint32_t backing_pages = (uint32_t)(gpu_framebuffer_size / 4096);
    gpu_framebuffer = (uint32_t *)sys_mmap(gpu_framebuffer_size);
    if (!gpu_framebuffer ||
        dma_export_range(gpu_framebuffer, gpu_framebuffer_size, 0,
                         &gpu_framebuffer_dma_cap) < 0) {
        gpu_init_error = 6;
        return -1;
    }
    zero_bytes(gpu_framebuffer, gpu_framebuffer_size);

    virtio_gpu_ctrl_hdr_t resp;
    virtio_gpu_resource_create_2d_t create = {
        .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D},
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width = gpu_width,
        .height = gpu_height
    };
    if (gpu_command(&create, sizeof(create), &resp, sizeof(resp),
                    VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        gpu_init_error = 7;
        return -1;
    }

    virtio_gpu_resource_attach_backing_t *attach =
        (virtio_gpu_resource_attach_backing_t *)gpu_request;
    zero_bytes(attach, GPU_REQUEST_PAGES * 4096);
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = VIRTIO_GPU_RESOURCE_ID;
    attach->nr_entries = backing_pages;
    virtio_gpu_mem_entry_t *entries =
        (virtio_gpu_mem_entry_t *)(attach + 1);
    for (uint32_t i = 0; i < backing_pages; i++) {
        uint64_t page_pa = sys_dma_paddr(gpu_framebuffer_dma_cap,
                                         (uint64_t)i * 4096);
        if (!page_pa) {
            gpu_init_error = 8;
            return -1;
        }
        entries[i].addr = page_pa;
        entries[i].length = 4096;
        entries[i].padding = 0;
    }
    uint32_t attach_len = sizeof(*attach) +
                          backing_pages * sizeof(virtio_gpu_mem_entry_t);
    if (gpu_command(attach, attach_len, &resp, sizeof(resp),
                    VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        gpu_init_error = 9;
        return -1;
    }

    virtio_gpu_set_scanout_t scanout = {
        .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT},
        .rect = {0, 0, gpu_width, gpu_height},
        .scanout_id = 0,
        .resource_id = VIRTIO_GPU_RESOURCE_ID
    };
    if (gpu_command(&scanout, sizeof(scanout), &resp, sizeof(resp),
                    VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        gpu_init_error = 10;
        return -1;
    }
    if (gpu_present_rect_now(0, 0, gpu_width, gpu_height) < 0) {
        gpu_init_error = 11;
        return -1;
    }

    screen_width = gpu_width;
    screen_height = gpu_height;
    screen_stride = gpu_width;
    screen_format = DISPLAY_PIXEL_BGR_RESERVED_8BIT;
    screen_bytes = gpu_framebuffer_size;
    gpu_init_error = 0;
    return 0;
}

static const uint8_t *glyph(char c) {
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    return (c >= 'A' && c <= 'Z') ? letters[c - 'A'] : blank;
}

static void draw_text(uint32_t x, uint32_t y, const char *text,
                      uint32_t scale, uint32_t rgb) {
    while (*text) {
        const uint8_t *rows = glyph(*text++);
        for (uint32_t row = 0; row < 7; row++) {
            for (uint32_t col = 0; col < 5; col++) {
                if (rows[row] & (1U << (4 - col))) {
                    ramfb_fill_rect(x + col * scale, y + row * scale,
                                    scale, scale, rgb);
                }
            }
        }
        x += 6 * scale;
    }
}

static void draw_boot_screen(void) {
    ramfb_fill_rect(0, 0, display->width, display->height, 0x000000);

    uint32_t scale = display->width >= 900 ? 3 : 2;
    uint32_t x = 20;
    uint32_t y = 20;
    uint32_t line_height = 10 * scale;
    uint32_t ok = 0x63D297;
    uint32_t text = 0xD8DEE9;

    draw_text(x, y, "NEPTUNE BOOT LOG", scale, text);
    y += line_height * 2;
    draw_text(x, y, "OK  UEFI FRAMEBUFFER READY", scale, ok);
    y += line_height;
    draw_text(x, y, "OK  DISPLAY SERVICE ONLINE", scale, ok);
    y += line_height;
    draw_text(x, y, "OK  FRAMEBUFFER MAPPED IN EL ZERO", scale, ok);
    y += line_height;
    draw_text(x, y, "OK  WAITING FOR COMPOSITOR", scale, ok);
}

static const display_backend_t ramfb_backend = {
    DISPLAY_BACKEND_RAMFB,
    DISPLAY_FEATURE_FILL_RECT |
    DISPLAY_FEATURE_REGISTER_BUFFER |
    DISPLAY_FEATURE_BLIT_BUFFER |
    DISPLAY_FEATURE_CPU_RGB888 |
    DISPLAY_FEATURE_SYNC_PRESENT |
    DISPLAY_FEATURE_PRESENT_QUEUE,
    ramfb_fill_rect,
    ramfb_register_buffer,
    ramfb_blit_buffer
};

static const display_backend_t virtio_gpu_backend = {
    DISPLAY_BACKEND_VIRTIO_GPU,
    DISPLAY_FEATURE_FILL_RECT |
    DISPLAY_FEATURE_REGISTER_BUFFER |
    DISPLAY_FEATURE_BLIT_BUFFER |
    DISPLAY_FEATURE_CPU_RGB888 |
    DISPLAY_FEATURE_SYNC_PRESENT |
    DISPLAY_FEATURE_PRESENT_QUEUE,
    gpu_fill_rect,
    gpu_register_buffer,
    gpu_blit_buffer
};

static int handle_blit_buffer(uint32_t id, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height) {
    if (!backend || !backend->blit_buffer) {
        return -1;
    }
    int status = backend->blit_buffer(id, x, y, width, height);
    if (status == 0) {
        stat_blit_rects++;
    }
    return status;
}

static int drain_present_queue(void) {
    if (!present_queue ||
        present_queue->magic != DISPLAY_PRESENT_QUEUE_MAGIC ||
        present_queue->version != DISPLAY_PRESENT_QUEUE_VERSION) {
        return 0;
    }

    uint32_t capacity = present_queue->capacity;
    if (capacity == 0 || capacity > DISPLAY_PRESENT_QUEUE_CAPACITY) {
        return 0;
    }

    int did_work = 0;
    for (uint32_t i = 0; i < capacity &&
         present_queue->read_index != present_queue->write_index; i++) {
        uint32_t index = present_queue->read_index % capacity;
        display_present_cmd_t cmd = present_queue->commands[index];
        barrier();
        present_queue->read_index = (index + 1) % capacity;
        if (handle_blit_buffer(cmd.buffer_id, cmd.x, cmd.y,
                               cmd.width, cmd.height) == 0) {
            stat_present_queue_commands++;
        } else {
            stat_errors++;
        }
        did_work = 1;
    }
    return did_work;
}

static void reset_stats(void) {
    stat_fill_rects = 0;
    stat_registered_buffers = 0;
    stat_blit_rects = 0;
    stat_present_queue_commands = 0;
    stat_errors = 0;
    stat_gpu_commands = 0;
    stat_gpu_transfers = 0;
    stat_gpu_flushes = 0;
    stat_gpu_transfer_bytes = 0;
    stat_gpu_queued_rects = 0;
    stat_gpu_merged_rects = 0;
    stat_gpu_batch_flushes = 0;
    stat_gpu_max_dirty_rects = gpu_dirty_count;
}

void _start(void) {
    display = (display_boot_info_t *)DISPLAY_BOOT_INFO_VA;
    display_available =
        display->magic == DISPLAY_BOOT_MAGIC &&
        display->version == DISPLAY_BOOT_VERSION;

    if (display_available && virtio_gpu_init() == 0) {
        backend = &virtio_gpu_backend;
    } else if (display_available &&
               display->framebuffer_va != 0 && display->framebuffer_size != 0) {
        framebuffer = (volatile uint32_t *)display->framebuffer_va;
        backend = &ramfb_backend;
        screen_width = display->width;
        screen_height = display->height;
        screen_stride = display->pixels_per_scan_line;
        screen_format = display->pixel_format;
        screen_bytes = display->framebuffer_size;
        draw_boot_screen();
    }

    while (ns_register("display") < 0) {
        sys_sleep(10);
    }

    while (1) {
        uint64_t timeout_ms = gpu_dirty_count ? GPU_PRESENT_INTERVAL_MS :
                              (present_queue ? DISPLAY_PRESENT_QUEUE_POLL_MS :
                               1000ULL);
        ipc_msg_t msg = sys_ipc_recv_timeout(CAP_SELF, timeout_ms);
        if (msg.status < 0) {
            (void)drain_present_queue();
            (void)gpu_flush_queued(0);
            continue;
        }
        (void)drain_present_queue();
        int status = 0;
        uint64_t reply_len = 0;
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};

        if (display_available && msg.payload[0] == DISPLAY_REQ_STATS) {
            (void)gpu_flush_queued(1);
            reply[0] = stat_fill_rects;
            reply[1] = stat_registered_buffers;
            reply[2] = stat_blit_rects;
            reply[3] = stat_errors;
            reply[4] = backend ? backend->type : DISPLAY_BACKEND_NONE;
            reply[5] = (uint64_t)gpu_init_error;
            reply[6] = stat_gpu_commands;
            reply[7] = stat_gpu_transfers;
            reply[8] = stat_gpu_flushes;
            reply[9] = stat_gpu_transfer_bytes;
            reply[10] = stat_gpu_queued_rects;
            reply[11] = stat_gpu_merged_rects;
            reply[12] = stat_gpu_batch_flushes;
            reply[13] = stat_gpu_max_dirty_rects;
            reply[14] = stat_present_queue_commands;
            reply_len = 120;
        } else if (display_available && msg.payload[0] == DISPLAY_REQ_CAPS) {
            (void)gpu_flush_queued(1);
            reply[0] = backend ? backend->type : DISPLAY_BACKEND_NONE;
            reply[1] = backend ? backend->features : 0;
            reply[2] = DISPLAY_MAX_BUFFERS;
            reply[3] = (uint64_t)gpu_init_error;
            reply_len = 32;
        } else if (!display_available || !backend) {
            status = -1;
        } else if (msg.payload[0] == DISPLAY_REQ_INFO) {
            (void)gpu_flush_queued(1);
            reply[0] = display_pack_pair(screen_width, screen_height);
            reply[1] = display_pack_pair(screen_stride, screen_format);
            reply[2] = screen_bytes;
            reply_len = 24;
        } else if (msg.payload[4] == DISPLAY_REQ_BIND_PRESENT_QUEUE) {
            if (!msg.payload[0] || msg.payload[1] < sizeof(display_present_queue_t)) {
                status = -1;
            } else {
                present_queue = (display_present_queue_t *)msg.payload[0];
                present_queue->magic = DISPLAY_PRESENT_QUEUE_MAGIC;
                present_queue->version = DISPLAY_PRESENT_QUEUE_VERSION;
                present_queue->capacity = DISPLAY_PRESENT_QUEUE_CAPACITY;
                present_queue->read_index = 0;
                present_queue->write_index = 0;
                present_queue->dropped = 0;
                reply[0] = DISPLAY_PRESENT_QUEUE_CAPACITY;
                reply_len = 8;
            }
        } else if (msg.payload[0] == DISPLAY_REQ_CLEAR) {
            backend->fill_rect(0, 0, screen_width, screen_height,
                               (uint32_t)msg.payload[1]);
            stat_fill_rects++;
        } else if (msg.payload[0] == DISPLAY_REQ_FILL_RECT) {
            backend->fill_rect((uint32_t)(msg.payload[1] >> 32),
                               (uint32_t)msg.payload[1],
                               (uint32_t)(msg.payload[2] >> 32),
                               (uint32_t)msg.payload[2],
                               (uint32_t)msg.payload[3]);
            stat_fill_rects++;
        } else if (msg.payload[4] == DISPLAY_REQ_REGISTER_BUFFER) {
            int id = backend->register_buffer((const uint32_t *)msg.payload[0],
                                              msg.payload[1],
                                              (uint32_t)(msg.payload[5] >> 32),
                                              (uint32_t)msg.payload[5],
                                              (uint32_t)msg.payload[6]);
            status = id < 0 ? -1 : 0;
            reply[0] = id < 0 ? 0 : (uint64_t)id;
            reply_len = 8;
            if (status == 0) {
                stat_registered_buffers++;
            }
        } else if (msg.payload[0] == DISPLAY_REQ_BLIT_BUFFER) {
            status = handle_blit_buffer((uint32_t)msg.payload[1],
                                        (uint32_t)(msg.payload[2] >> 32),
                                        (uint32_t)msg.payload[2],
                                        (uint32_t)(msg.payload[3] >> 32),
                                        (uint32_t)msg.payload[3]);
        } else if (msg.payload[0] == DISPLAY_REQ_RESET_STATS) {
            (void)gpu_flush_queued(1);
            reset_stats();
        } else {
            status = -1;
        }

        if (gpu_dirty_count >= GPU_FULL_REPAINT_DIRTY_THRESHOLD) {
            (void)gpu_flush_queued(1);
        }
        (void)drain_present_queue();
        if (status < 0) {
            stat_errors++;
        }
        sys_ipc_reply(msg.reply_cap, status, 0, reply_len, reply);
    }
}
