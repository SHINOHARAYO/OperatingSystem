#include "lib.h"
#include "ns_proto.h"
#include "keyboard_proto.h"
#include "input_boot.h"

#define UART_BASE 0xB0000000
#define UART_DR ((volatile uint32_t *)(UART_BASE + 0x000))
#define UART_FR ((volatile uint32_t *)(UART_BASE + 0x018))

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
#define KEY_QUEUE_SIZE 128
#define KEYBOARD_READ_TIMEOUT_MS 5
#define VIRTIO_INPUT_CFG_EV_BITS 0x11
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_X 0
#define REL_Y 1
#define ABS_X 0
#define ABS_Y 1
#define KEY_ENTER 28
#define KEY_SPACE 57

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

static volatile uint8_t *virtio_mmio;
static virtq_desc_t *vq_desc;
static virtq_avail_t *vq_avail;
static virtq_used_t *vq_used;
static virtio_input_event_t *events;
static uint16_t vq_used_idx;
static int virtio_ready;
static int shift_down;
static int ctrl_down;
static int alt_down;
static uint64_t key_queue[KEY_QUEUE_SIZE];
static uint32_t key_head;
static uint32_t key_tail;
static uint32_t focus_manager_tid;
static uint32_t focused_tid;

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

static void queue_event(uint64_t event) {
    uint32_t next = (key_tail + 1) % KEY_QUEUE_SIZE;
    if (next != key_head) {
        key_queue[key_tail] = event;
        key_tail = next;
    }
}

static uint64_t dequeue_event(void) {
    if (key_head == key_tail) {
        return 0;
    }
    uint64_t event = key_queue[key_head];
    key_head = (key_head + 1) % KEY_QUEUE_SIZE;
    return event;
}

static char keycode_to_char(uint16_t code) {
    static const char letters[26] = "abcdefghijklmnopqrstuvwxyz";
    static const uint16_t letter_codes[26] = {
        30,48,46,32,18,33,34,35,23,36,37,38,50,
        49,24,25,16,19,31,20,22,47,17,45,21,44
    };
    for (uint32_t i = 0; i < 26; i++) {
        if (code == letter_codes[i]) {
            char c = letters[i];
            if (ctrl_down) {
                return (char)(c - 'a' + 1);
            }
            return shift_down ? (char)(c - 'a' + 'A') : c;
        }
    }
    static const char digits[] = "1234567890";
    if (code >= 2 && code <= 11) return digits[code - 2];
    if (code == 28) return '\n';
    if (code == 14) return '\b';
    if (code == 57) return ' ';
    if (code == 52) return '.';
    if (code == 53) return '/';
    if (code == 12) return '-';
    return 0;
}

static void handle_input_event(virtio_input_event_t event) {
    if (event.type != 1) {
        return;
    }
    if (event.code == 42 || event.code == 54) {
        shift_down = event.value != 0;
    } else if (event.code == 29 || event.code == 97) {
        ctrl_down = event.value != 0;
    } else if (event.code == 56 || event.code == 100) {
        alt_down = event.value != 0;
    }
    char c = (event.value == KEYBOARD_KEY_PRESS ||
              event.value == KEYBOARD_KEY_REPEAT) ?
             keycode_to_char(event.code) : 0;
    uint64_t modifiers = (shift_down ? KEYBOARD_MOD_SHIFT : 0) |
                         (ctrl_down ? KEYBOARD_MOD_CTRL : 0) |
                         (alt_down ? KEYBOARD_MOD_ALT : 0);
    queue_event(KEYBOARD_EVENT_PACK(KEYBOARD_EVENT_KEY, modifiers,
                event.value, event.code, c));
}

static int virtio_keyboard_init(void) {
    int found = 0;
    for (uint64_t off = 0; off < INPUT_DEVICE_MMIO_SIZE; off += 0x200) {
        virtio_mmio = (volatile uint8_t *)(INPUT_DEVICE_MMIO_VA + off);
        if (mmio_read32(VIRTIO_MMIO_MAGIC) == 0x74726976 &&
            mmio_read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_INPUT_DEVICE_ID) {
            int has_keyboard_keys =
                input_has_event_type(EV_KEY) ||
                input_has_event_code(EV_KEY, KEY_ENTER) ||
                input_has_event_code(EV_KEY, KEY_SPACE);
            int has_pointer_axes =
                input_has_event_type(EV_REL) ||
                input_has_event_type(EV_ABS) ||
                input_has_event_code(EV_REL, REL_X) ||
                input_has_event_code(EV_REL, REL_Y) ||
                input_has_event_code(EV_ABS, ABS_X) ||
                input_has_event_code(EV_ABS, ABS_Y);
            if (has_keyboard_keys && !has_pointer_axes) {
                found = 1;
                break;
            }
        }
        virtio_mmio = 0;
    }
    if (!found || !virtio_mmio || mmio_read32(VIRTIO_MMIO_VERSION) != 2) {
        return -1;
    }

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

static void poll_virtio_keyboard(void) {
    if (!virtio_ready) return;
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
    if (reposted) mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
}

static void poll_uart(void) {
    while ((*UART_FR & (1 << 4)) == 0) {
        char c = (char)(*UART_DR & 0xFF);
        queue_event(KEYBOARD_EVENT_PACK(KEYBOARD_EVENT_KEY, 0,
                                        KEYBOARD_KEY_PRESS, 0, c));
    }
}

static uint64_t read_event(void) {
    poll_uart();
    poll_virtio_keyboard();
    uint64_t event = dequeue_event();
    if (event) {
        return event;
    }

    sys_sleep(KEYBOARD_READ_TIMEOUT_MS);
    poll_uart();
    poll_virtio_keyboard();
    return dequeue_event();
}

void _start(void) {
#if NEPTUNE_PLATFORM_PI4
    /* The Pi 4 bootstrap uses the existing PL011 polling path only. */
#else
    (void)virtio_keyboard_init();
#endif
    while (ns_register("keyboard") < 0) {
        sys_sleep(10);
    }

    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
        int status = 0;
        if (msg.payload[0] == KEYBOARD_REQ_READ) {
            if (focused_tid != 0 && msg.sender_tid != focused_tid) {
                status = -1;
            } else {
                reply[0] = read_event();
            }
        } else if (msg.payload[0] == KEYBOARD_REQ_BIND_MANAGER) {
            if (focus_manager_tid != 0 && focus_manager_tid != msg.sender_tid) {
                status = -1;
            } else {
                focus_manager_tid = msg.sender_tid;
            }
        } else if (msg.payload[0] == KEYBOARD_REQ_SET_FOCUS) {
            if (focus_manager_tid == 0 || msg.sender_tid != focus_manager_tid) {
                status = -1;
            } else {
                focused_tid = (uint32_t)msg.payload[1];
                key_head = key_tail;
            }
        } else {
            status = -1;
        }
        sys_ipc_reply(msg.reply_cap, status, 0, 8, reply);
    }
}
