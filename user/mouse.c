#include "lib.h"
#include "ns_proto.h"
#include "mouse_proto.h"
#include "input_boot.h"

#define VIRTIO_MMIO_MAGIC        0x000
#define VIRTIO_MMIO_VERSION      0x004
#define VIRTIO_MMIO_DEVICE_ID    0x008
#define VIRTIO_MMIO_DEVICE_FEAT  0x010
#define VIRTIO_MMIO_DEVICE_SEL   0x014
#define VIRTIO_MMIO_DRIVER_FEAT  0x020
#define VIRTIO_MMIO_DRIVER_SEL   0x024
#define VIRTIO_MMIO_QUEUE_SEL    0x030
#define VIRTIO_MMIO_QUEUE_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM    0x038
#define VIRTIO_MMIO_QUEUE_READY  0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_STATUS       0x070
#define VIRTIO_MMIO_DESC_LOW     0x080
#define VIRTIO_MMIO_DESC_HIGH    0x084
#define VIRTIO_MMIO_DRIVER_LOW   0x090
#define VIRTIO_MMIO_DRIVER_HIGH  0x094
#define VIRTIO_MMIO_DEVICE_LOW   0x0A0
#define VIRTIO_MMIO_DEVICE_HIGH  0x0A4
#define VIRTIO_INPUT_CFG_SELECT  0x100
#define VIRTIO_INPUT_CFG_SUBSEL  0x101
#define VIRTIO_INPUT_CFG_SIZE    0x102
#define VIRTIO_INPUT_CFG_DATA    0x108

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_F_VERSION_1_LOW_BIT 0
#define VIRTIO_INPUT_DEVICE_ID 18
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_SIZE 32
#define POINTER_QUEUE_SIZE 128

#define VIRTIO_INPUT_CFG_EV_BITS  0x11
#define VIRTIO_INPUT_CFG_ABS_INFO 0x12
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define SYN_REPORT 0
#define REL_X 0
#define REL_Y 1
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define MOUSE_SERVICE_POLL_MS 2ULL

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
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

typedef struct {
    uint64_t event;
    uint32_t x;
    uint32_t y;
} pointer_event_t;

static volatile uint8_t *virtio_mmio;
static virtq_desc_t *vq_desc;
static virtq_avail_t *vq_avail;
static virtq_used_t *vq_used;
static virtio_input_event_t *events;
static uint16_t vq_used_idx;
static int virtio_ready;
static uint32_t pointer_x;
static uint32_t pointer_y;
static uint32_t pointer_max_x = 32767;
static uint32_t pointer_max_y = 32767;
static uint64_t pointer_buttons;
static int pointer_dirty;
static int pointer_absolute;
static pointer_event_t pointer_queue[POINTER_QUEUE_SIZE];
static uint32_t pointer_head;
static uint32_t pointer_tail;
static mouse_ring_t *shared_ring;
static uint64_t stat_found;
static uint64_t stat_read_requests;
static uint64_t stat_raw_events;
static uint64_t stat_queued_events;
static uint64_t stat_dropped_events;

static uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(virtio_mmio + off);
}

static void mmio_write32(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(virtio_mmio + off) = value;
}

static void mmio_write8(uint32_t off, uint8_t value) {
    *(volatile uint8_t *)(virtio_mmio + off) = value;
}

static uint8_t mmio_read8(uint32_t off) {
    return *(volatile uint8_t *)(virtio_mmio + off);
}

static void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static void zero_bytes(void *ptr, uint64_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void *dma_page(uint64_t *out_pa) {
    uint8_t *page = (uint8_t *)sys_mmap(4096);
    if (!page) {
        return 0;
    }
    page[0] = 0;
    int cap = sys_dma_export(page, 4096, OCAP_RIGHT_READ | OCAP_RIGHT_WRITE);
    if (cap < 0) {
        return 0;
    }
    uint64_t pa = sys_dma_paddr((uint32_t)cap, 0);
    if (!pa) {
        return 0;
    }
    zero_bytes(page, 4096);
    *out_pa = pa;
    return page;
}

static int input_config_bit(uint8_t select, uint8_t subsel, uint16_t bit) {
    mmio_write8(VIRTIO_INPUT_CFG_SELECT, select);
    mmio_write8(VIRTIO_INPUT_CFG_SUBSEL, subsel);
    barrier();
    uint8_t size = mmio_read8(VIRTIO_INPUT_CFG_SIZE);
    uint16_t byte = bit / 8;
    if (byte >= size) {
        return 0;
    }
    uint8_t value = mmio_read8(VIRTIO_INPUT_CFG_DATA + byte);
    return (value & (1U << (bit & 7U))) != 0;
}

static int input_has_event_type(uint16_t type) {
    return input_config_bit(VIRTIO_INPUT_CFG_EV_BITS, 0, type);
}

static int input_has_event_code(uint16_t type, uint16_t code) {
    return input_config_bit(VIRTIO_INPUT_CFG_EV_BITS, (uint8_t)type, code);
}

static uint32_t input_config_u32(uint32_t off) {
    uint32_t value = 0;
    value |= (uint32_t)mmio_read8(off);
    value |= (uint32_t)mmio_read8(off + 1) << 8;
    value |= (uint32_t)mmio_read8(off + 2) << 16;
    value |= (uint32_t)mmio_read8(off + 3) << 24;
    return value;
}

static uint32_t input_abs_max(uint8_t axis) {
    mmio_write8(VIRTIO_INPUT_CFG_SELECT, VIRTIO_INPUT_CFG_ABS_INFO);
    mmio_write8(VIRTIO_INPUT_CFG_SUBSEL, axis);
    barrier();
    if (mmio_read8(VIRTIO_INPUT_CFG_SIZE) < 8) {
        return 32767;
    }
    uint32_t max = input_config_u32(VIRTIO_INPUT_CFG_DATA + 4);
    return max == 0 ? 32767 : max;
}

static void queue_pointer_event(void) {
    if (shared_ring && shared_ring->magic == MOUSE_RING_MAGIC &&
        shared_ring->version == MOUSE_RING_VERSION &&
        shared_ring->capacity == MOUSE_RING_CAPACITY) {
        uint32_t write = shared_ring->write_index;
        uint32_t next = (write + 1) % MOUSE_RING_CAPACITY;
        if (next != shared_ring->read_index) {
            shared_ring->events[write].event =
                MOUSE_EVENT_PACK(MOUSE_EVENT_POINTER, pointer_buttons);
            shared_ring->events[write].x = pointer_x;
            shared_ring->events[write].y = pointer_y;
            barrier();
            shared_ring->write_index = next;
            stat_queued_events++;
        } else {
            shared_ring->dropped++;
            stat_dropped_events++;
        }
        return;
    }

    uint32_t next = (pointer_tail + 1) % POINTER_QUEUE_SIZE;
    if (next != pointer_head) {
        pointer_queue[pointer_tail].event =
            MOUSE_EVENT_PACK(MOUSE_EVENT_POINTER, pointer_buttons);
        pointer_queue[pointer_tail].x = pointer_x;
        pointer_queue[pointer_tail].y = pointer_y;
        pointer_tail = next;
        stat_queued_events++;
    } else {
        stat_dropped_events++;
    }
}

static pointer_event_t dequeue_pointer_event(void) {
    pointer_event_t event = {0, 0, 0};
    if (pointer_head == pointer_tail) {
        return event;
    }
    event = pointer_queue[pointer_head];
    pointer_head = (pointer_head + 1) % POINTER_QUEUE_SIZE;
    return event;
}

static void handle_input_event(virtio_input_event_t event) {
    stat_raw_events++;
    if (event.type == EV_ABS) {
        if (event.code == ABS_X) {
            pointer_x = event.value;
            pointer_dirty = 1;
        } else if (event.code == ABS_Y) {
            pointer_y = event.value;
            pointer_dirty = 1;
        }
    } else if (event.type == EV_REL) {
        int32_t value = (int32_t)event.value;
        if (event.code == REL_X) {
            if (value < 0 && pointer_x < (uint32_t)(-value)) {
                pointer_x = 0;
            } else {
                pointer_x = (uint32_t)((int32_t)pointer_x + value);
                if (pointer_x > pointer_max_x) {
                    pointer_x = pointer_max_x;
                }
            }
            pointer_dirty = 1;
        } else if (event.code == REL_Y) {
            if (value < 0 && pointer_y < (uint32_t)(-value)) {
                pointer_y = 0;
            } else {
                pointer_y = (uint32_t)((int32_t)pointer_y + value);
                if (pointer_y > pointer_max_y) {
                    pointer_y = pointer_max_y;
                }
            }
            pointer_dirty = 1;
        }
    } else if (event.type == EV_KEY) {
        uint64_t button = 0;
        if (event.code == BTN_LEFT) {
            button = MOUSE_BUTTON_LEFT;
        } else if (event.code == BTN_RIGHT) {
            button = MOUSE_BUTTON_RIGHT;
        } else if (event.code == BTN_MIDDLE) {
            button = MOUSE_BUTTON_MIDDLE;
        }
        if (button) {
            if (event.value) {
                pointer_buttons |= button;
            } else {
                pointer_buttons &= ~button;
            }
            pointer_dirty = 1;
        }
    } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
        if (pointer_dirty) {
            queue_pointer_event();
            pointer_dirty = 0;
        }
    }
}

static int virtio_mouse_init(void) {
    volatile uint8_t *relative_mmio = 0;
    volatile uint8_t *absolute_mmio = 0;
    for (uint64_t off = 0; off < INPUT_DEVICE_MMIO_SIZE; off += 0x200) {
        virtio_mmio = (volatile uint8_t *)(INPUT_DEVICE_MMIO_VA + off);
        if (mmio_read32(VIRTIO_MMIO_MAGIC) == 0x74726976 &&
            mmio_read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_INPUT_DEVICE_ID) {
            int has_rel_x = input_has_event_code(EV_REL, REL_X);
            int has_rel_y = input_has_event_code(EV_REL, REL_Y);
            int has_abs_x = input_has_event_code(EV_ABS, ABS_X);
            int has_abs_y = input_has_event_code(EV_ABS, ABS_Y);
            if (has_rel_x && has_rel_y && !relative_mmio) {
                relative_mmio = virtio_mmio;
            }
            if (has_abs_x && has_abs_y && !absolute_mmio) {
                absolute_mmio = virtio_mmio;
            }
        }
        virtio_mmio = 0;
    }

    pointer_absolute = absolute_mmio != 0;
    virtio_mmio = pointer_absolute ? absolute_mmio : relative_mmio;
    if (!virtio_mmio || mmio_read32(VIRTIO_MMIO_VERSION) != 2) {
        printf("mouse: no virtio pointer device found\n");
        return -1;
    }
    stat_found = 1;

    if (pointer_absolute) {
        pointer_max_x = input_abs_max(ABS_X);
        pointer_max_y = input_abs_max(ABS_Y);
    } else {
        pointer_max_x = 32767;
        pointer_max_y = 32767;
        pointer_x = pointer_max_x / 2;
        pointer_y = pointer_max_y / 2;
        pointer_dirty = 1;
    }

    printf("mouse: using %s pointer max=%u,%u\n",
           pointer_absolute ? "absolute" : "relative",
           pointer_max_x, pointer_max_y);

    mmio_write32(VIRTIO_MMIO_STATUS, 0);
    mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                      VIRTIO_STATUS_DRIVER);
    mmio_write32(VIRTIO_MMIO_DEVICE_SEL, 1);
    if (!(mmio_read32(VIRTIO_MMIO_DEVICE_FEAT) &
          (1U << VIRTIO_F_VERSION_1_LOW_BIT))) {
        return -1;
    }
    mmio_write32(VIRTIO_MMIO_DRIVER_SEL, 0);
    mmio_write32(VIRTIO_MMIO_DRIVER_FEAT, 0);
    mmio_write32(VIRTIO_MMIO_DRIVER_SEL, 1);
    mmio_write32(VIRTIO_MMIO_DRIVER_FEAT, 1U << VIRTIO_F_VERSION_1_LOW_BIT);
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK;
    mmio_write32(VIRTIO_MMIO_STATUS, status);
    if (!(mmio_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        return -1;
    }

    uint64_t queue_pa = 0;
    uint64_t events_pa = 0;
    uint8_t *queue_page = (uint8_t *)dma_page(&queue_pa);
    events = (virtio_input_event_t *)dma_page(&events_pa);
    if (!queue_page || !events) {
        return -1;
    }
    vq_desc = (virtq_desc_t *)queue_page;
    vq_avail = (virtq_avail_t *)(queue_page + sizeof(virtq_desc_t) * VIRTQ_SIZE);
    vq_used = (virtq_used_t *)(queue_page + 2048);

    mmio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    if (mmio_read32(VIRTIO_MMIO_QUEUE_MAX) < VIRTQ_SIZE) {
        return -1;
    }
    mmio_write32(VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
    mmio_write32(VIRTIO_MMIO_DESC_LOW, (uint32_t)queue_pa);
    mmio_write32(VIRTIO_MMIO_DESC_HIGH, (uint32_t)(queue_pa >> 32));
    uint64_t avail_pa = queue_pa + sizeof(virtq_desc_t) * VIRTQ_SIZE;
    uint64_t used_pa = queue_pa + 2048;
    mmio_write32(VIRTIO_MMIO_DRIVER_LOW, (uint32_t)avail_pa);
    mmio_write32(VIRTIO_MMIO_DRIVER_HIGH, (uint32_t)(avail_pa >> 32));
    mmio_write32(VIRTIO_MMIO_DEVICE_LOW, (uint32_t)used_pa);
    mmio_write32(VIRTIO_MMIO_DEVICE_HIGH, (uint32_t)(used_pa >> 32));
    for (uint32_t i = 0; i < VIRTQ_SIZE; i++) {
        vq_desc[i].addr = events_pa + i * sizeof(virtio_input_event_t);
        vq_desc[i].len = sizeof(virtio_input_event_t);
        vq_desc[i].flags = VIRTQ_DESC_F_WRITE;
        vq_avail->ring[i] = (uint16_t)i;
    }
    barrier();
    vq_avail->idx = VIRTQ_SIZE;
    mmio_write32(VIRTIO_MMIO_QUEUE_READY, 1);
    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(VIRTIO_MMIO_STATUS, status);
    mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    virtio_ready = 1;
    return 0;
}

static void poll_virtio_mouse(void) {
    if (!virtio_ready) {
        return;
    }
    int reposted = 0;
    while (vq_used_idx != vq_used->idx) {
        barrier();
        uint32_t id = vq_used->ring[vq_used_idx % VIRTQ_SIZE].id;
        if (id < VIRTQ_SIZE) {
            handle_input_event(events[id]);
            vq_avail->ring[vq_avail->idx % VIRTQ_SIZE] = (uint16_t)id;
            barrier();
            vq_avail->idx++;
            reposted = 1;
        }
        vq_used_idx++;
    }
    if (pointer_dirty) {
        queue_pointer_event();
        pointer_dirty = 0;
    }
    if (reposted) {
        mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    }
}

void _start(void) {
    (void)virtio_mouse_init();
    while (ns_register("mouse") < 0) {
        sys_sleep(10);
    }

    while (1) {
        ipc_msg_t msg = sys_ipc_recv_timeout(CAP_SELF, MOUSE_SERVICE_POLL_MS);
        poll_virtio_mouse();
        if (msg.status < 0) {
            continue;
        }
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
        uint64_t reply_len = 0;
        int status = 0;
        if (msg.payload[0] == MOUSE_REQ_INFO) {
            reply[0] = mouse_pack_pair(pointer_max_x, pointer_max_y);
            reply[1] = virtio_ready ? 1 : 0;
            reply[2] = pointer_absolute ? 1 : 0;
            reply_len = 24;
        } else if (msg.payload[0] == MOUSE_REQ_READ) {
            stat_read_requests++;
            poll_virtio_mouse();
            pointer_event_t event = dequeue_pointer_event();
            reply[0] = event.event;
            reply[1] = mouse_pack_pair(event.x, event.y);
            reply_len = 16;
        } else if (msg.payload[0] == MOUSE_REQ_STATS) {
            reply[0] = stat_found;
            reply[1] = pointer_absolute ? 1 : 0;
            reply[2] = stat_read_requests;
            reply[3] = stat_raw_events;
            reply[4] = stat_queued_events;
            reply[5] = stat_dropped_events;
            reply[6] = mouse_pack_pair(pointer_x, pointer_y);
            reply[7] = pointer_buttons;
            reply_len = 64;
        } else if (msg.payload[4] == MOUSE_REQ_BIND_RING) {
            if (!msg.payload[0] || msg.payload[1] < sizeof(mouse_ring_t)) {
                status = -1;
            } else {
                shared_ring = (mouse_ring_t *)msg.payload[0];
                shared_ring->magic = MOUSE_RING_MAGIC;
                shared_ring->version = MOUSE_RING_VERSION;
                shared_ring->capacity = MOUSE_RING_CAPACITY;
                shared_ring->read_index = 0;
                shared_ring->write_index = 0;
                shared_ring->dropped = 0;
                reply[0] = MOUSE_RING_CAPACITY;
                reply_len = 8;
            }
        } else {
            status = -1;
        }
        sys_ipc_reply(msg.reply_cap, status, 0, reply_len, reply);
    }
}
