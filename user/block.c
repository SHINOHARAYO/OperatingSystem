#include "lib.h"
#include "fs_proto.h"
#include "ipc_proto.h"
#include "ns_proto.h"
#include "block_proto.h"

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
#define VIRTIO_MMIO_QUEUE_ALIGN  0x03C
#define VIRTIO_MMIO_QUEUE_PFN    0x040
#define VIRTIO_MMIO_QUEUE_READY  0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_IRQ_STATUS   0x060
#define VIRTIO_MMIO_IRQ_ACK      0x064
#define VIRTIO_MMIO_STATUS       0x070
#define VIRTIO_MMIO_DESC_LOW     0x080
#define VIRTIO_MMIO_DESC_HIGH    0x084
#define VIRTIO_MMIO_DRIVER_LOW   0x090
#define VIRTIO_MMIO_DRIVER_HIGH  0x094
#define VIRTIO_MMIO_DEVICE_LOW   0x0A0
#define VIRTIO_MMIO_DEVICE_HIGH  0x0A4
#define VIRTIO_MMIO_CONFIG       0x100

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1_LOW_BIT 0
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK  0
#define VIRTQ_SIZE 8

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
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_t;

static unsigned char *disk_data;
static uint64_t disk_size;
static int virtio_ready;
static volatile uint8_t *virtio_mmio = (volatile uint8_t *)BLOCK_VIRTIO_MMIO_VA;
static virtq_desc_t *vq_desc;
static virtq_avail_t *vq_avail;
static virtq_used_t *vq_used;
static virtio_blk_req_t *vq_req;
static unsigned char *vq_status;
static uint64_t vq_desc_pa;
static uint64_t vq_avail_pa;
static uint64_t vq_used_pa;
static uint64_t vq_req_pa;
static uint16_t vq_avail_idx;
static uint16_t vq_used_idx;
static int virtio_modern;

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(virtio_mmio + off);
}

static void mmio_write32(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(virtio_mmio + off) = value;
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

static void *dma_page(uint64_t *out_pa) {
    unsigned char *page = (unsigned char *)sys_mmap(4096);
    if (!page) {
        return 0;
    }

    page[0] = 0;
    uint64_t pa = sys_dma_paddr(page);
    if (!pa) {
        sys_munmap(page, 4096);
        return 0;
    }

    zero_bytes(page, 4096);
    *out_pa = pa;
    return page;
}

static void *dma_pages(uint64_t page_count, uint64_t *out_pa) {
    unsigned char *mem = (unsigned char *)sys_mmap(page_count * 4096);
    if (!mem) {
        return 0;
    }

    uint64_t base_pa = 0;
    for (uint64_t i = 0; i < page_count; i++) {
        unsigned char *page = mem + (i * 4096);
        page[0] = 0;
        uint64_t pa = sys_dma_paddr(page);
        if (!pa || (i > 0 && pa != base_pa + (i * 4096))) {
            sys_munmap(mem, page_count * 4096);
            return 0;
        }
        if (i == 0) {
            base_pa = pa;
        }
    }

    zero_bytes(mem, page_count * 4096);
    *out_pa = base_pa;
    return mem;
}

static int setup_modern_queue(void) {
    mmio_write32(VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
    mmio_write32(VIRTIO_MMIO_DESC_LOW, (uint32_t)vq_desc_pa);
    mmio_write32(VIRTIO_MMIO_DESC_HIGH, (uint32_t)(vq_desc_pa >> 32));
    mmio_write32(VIRTIO_MMIO_DRIVER_LOW, (uint32_t)vq_avail_pa);
    mmio_write32(VIRTIO_MMIO_DRIVER_HIGH, (uint32_t)(vq_avail_pa >> 32));
    mmio_write32(VIRTIO_MMIO_DEVICE_LOW, (uint32_t)vq_used_pa);
    mmio_write32(VIRTIO_MMIO_DEVICE_HIGH, (uint32_t)(vq_used_pa >> 32));
    mmio_write32(VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int setup_legacy_queue(void) {
    uint64_t vq_mem_pa = 0;
    unsigned char *vq_mem = (unsigned char *)dma_pages(2, &vq_mem_pa);
    if (!vq_mem) {
        return -1;
    }

    vq_desc = (virtq_desc_t *)vq_mem;
    vq_avail = (virtq_avail_t *)(vq_mem + (sizeof(virtq_desc_t) * VIRTQ_SIZE));
    vq_used = (virtq_used_t *)(vq_mem + 4096);
    vq_desc_pa = vq_mem_pa;
    vq_avail_pa = vq_mem_pa + (sizeof(virtq_desc_t) * VIRTQ_SIZE);
    vq_used_pa = vq_mem_pa + 4096;

    mmio_write32(VIRTIO_MMIO_QUEUE_NUM, VIRTQ_SIZE);
    mmio_write32(VIRTIO_MMIO_QUEUE_ALIGN, 4096);
    mmio_write32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_mem_pa >> 12));
    return 0;
}

static int virtio_blk_init(void) {
    virtio_mmio = 0;
    for (uint64_t off = 0; off < BLOCK_VIRTIO_MMIO_SIZE; off += 0x200) {
        volatile uint8_t *candidate = (volatile uint8_t *)(BLOCK_VIRTIO_MMIO_VA + off);
        virtio_mmio = candidate;
        if (mmio_read32(VIRTIO_MMIO_MAGIC) == 0x74726976 &&
            mmio_read32(VIRTIO_MMIO_DEVICE_ID) == 2) {
            break;
        }
        virtio_mmio = 0;
    }
    if (!virtio_mmio) {
        virtio_mmio = (volatile uint8_t *)BLOCK_VIRTIO_MMIO_VA;
        return -1;
    }

    uint32_t version = mmio_read32(VIRTIO_MMIO_VERSION);
    if (mmio_read32(VIRTIO_MMIO_MAGIC) != 0x74726976 ||
        (version != 1 && version != 2) ||
        mmio_read32(VIRTIO_MMIO_DEVICE_ID) != 2) {
        return -1;
    }
    virtio_modern = version == 2;

    mmio_write32(VIRTIO_MMIO_STATUS, 0);
    barrier();
    mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    if (virtio_modern) {
        mmio_write32(VIRTIO_MMIO_DEVICE_SEL, 1);
        uint32_t features_hi = mmio_read32(VIRTIO_MMIO_DEVICE_FEAT);
        if (!(features_hi & (1U << VIRTIO_F_VERSION_1_LOW_BIT))) {
            mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }

        mmio_write32(VIRTIO_MMIO_DRIVER_SEL, 0);
        mmio_write32(VIRTIO_MMIO_DRIVER_FEAT, 0);
        mmio_write32(VIRTIO_MMIO_DRIVER_SEL, 1);
        mmio_write32(VIRTIO_MMIO_DRIVER_FEAT, 1U << VIRTIO_F_VERSION_1_LOW_BIT);

        status |= VIRTIO_STATUS_FEATURES_OK;
        mmio_write32(VIRTIO_MMIO_STATUS, status);
        if (!(mmio_read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
    } else {
        mmio_write32(VIRTIO_MMIO_DRIVER_SEL, 0);
        mmio_write32(VIRTIO_MMIO_DRIVER_FEAT, 0);
    }

    mmio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = mmio_read32(VIRTIO_MMIO_QUEUE_MAX);
    if (qmax < VIRTQ_SIZE) {
        mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    if (virtio_modern) {
        vq_desc = (virtq_desc_t *)dma_page(&vq_desc_pa);
        vq_avail = (virtq_avail_t *)dma_page(&vq_avail_pa);
        vq_used = (virtq_used_t *)dma_page(&vq_used_pa);
    } else if (setup_legacy_queue() < 0) {
        mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    vq_req = (virtio_blk_req_t *)dma_page(&vq_req_pa);
    if (!vq_desc || !vq_avail || !vq_used || !vq_req) {
        mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    vq_status = (unsigned char *)vq_req + 32;

    if (virtio_modern && setup_modern_queue() < 0) {
        mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    uint64_t sectors = (uint64_t)mmio_read32(VIRTIO_MMIO_CONFIG) |
                       ((uint64_t)mmio_read32(VIRTIO_MMIO_CONFIG + 4) << 32);
    if (sectors == 0) {
        mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }
    disk_size = sectors * BLOCK_SECTOR_SIZE;

    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(VIRTIO_MMIO_STATUS, status);
    barrier();
    virtio_ready = 1;
    return 0;
}

static int virtio_blk_rw(uint64_t sector, uint64_t bytes, char *remote, int write) {
    uint64_t data_pa = sys_dma_paddr(remote);
    if (!virtio_ready || !data_pa || bytes == 0 || (bytes & (BLOCK_SECTOR_SIZE - 1)) != 0) {
        return -1;
    }

    vq_req->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    vq_req->reserved = 0;
    vq_req->sector = sector;
    *vq_status = 0xFF;

    vq_desc[0].addr = vq_req_pa;
    vq_desc[0].len = sizeof(virtio_blk_req_t);
    vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[0].next = 1;

    vq_desc[1].addr = data_pa;
    vq_desc[1].len = (uint32_t)bytes;
    vq_desc[1].flags = VIRTQ_DESC_F_NEXT | (write ? 0 : VIRTQ_DESC_F_WRITE);
    vq_desc[1].next = 2;

    vq_desc[2].addr = vq_req_pa + 32;
    vq_desc[2].len = 1;
    vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[2].next = 0;

    vq_avail->ring[vq_avail_idx % VIRTQ_SIZE] = 0;
    barrier();
    vq_avail_idx++;
    vq_avail->idx = vq_avail_idx;
    barrier();
    mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    for (uint64_t spins = 0; spins < 10000000; spins++) {
        barrier();
        if (vq_used->idx != vq_used_idx) {
            vq_used_idx = vq_used->idx;
            uint32_t irq_status = mmio_read32(VIRTIO_MMIO_IRQ_STATUS);
            if (irq_status) {
                mmio_write32(VIRTIO_MMIO_IRQ_ACK, irq_status);
            }
            return *vq_status == VIRTIO_BLK_S_OK ? 0 : -1;
        }
    }

    return -1;
}

static int find_apps_fat(void) {
    const unsigned char *image = (const unsigned char *)USER_BOOT_INITRD_BASE;
    const initrd_header_t *header = (const initrd_header_t *)image;
    if (header->magic != INITRD_MAGIC ||
        header->version != INITRD_VERSION ||
        header->file_count == 0 ||
        header->header_size < sizeof(initrd_header_t) +
                              ((uint64_t)header->file_count * sizeof(initrd_entry_t)) ||
        header->header_size > USER_BOOT_INITRD_MAX_SIZE) {
        return -1;
    }

    const initrd_entry_t *entries =
        (const initrd_entry_t *)(image + sizeof(initrd_header_t));
    for (uint32_t i = 0; i < header->file_count; i++) {
        const initrd_entry_t *entry = &entries[i];
        if (entry->offset > USER_BOOT_INITRD_MAX_SIZE ||
            entry->size > USER_BOOT_INITRD_MAX_SIZE - entry->offset ||
            entry->name[0] == '\0') {
            return -1;
        }
        if (streq(entry->name, "apps.fat")) {
            disk_size = entry->size;
            if (disk_size < BLOCK_SECTOR_SIZE) {
                return -1;
            }

            uint64_t aligned_size = page_align_size(disk_size);
            disk_data = (unsigned char *)sys_mmap(aligned_size);
            if (!disk_data) {
                return -1;
            }
            copy_bytes(disk_data, image + entry->offset, disk_size);
            return 0;
        }
    }

    return -1;
}

static int validate_io(ipc_msg_t msg, char **remote_out,
                       uint64_t *bytes_out, uint64_t *offset_out) {
    char *remote = (char *)msg.payload[0];
    uint64_t mapped_size = msg.payload[1];
    uint64_t sector = msg.payload[5];
    uint64_t count = msg.payload[6];
    uint64_t max_u64 = ~0ULL;
    uint64_t bytes = 0;
    uint64_t offset = 0;

    int ok = remote &&
             count != 0 &&
             count <= 8 &&
             count <= max_u64 / BLOCK_SECTOR_SIZE &&
             sector <= max_u64 / BLOCK_SECTOR_SIZE;

    if (ok) {
        bytes = count * BLOCK_SECTOR_SIZE;
        offset = sector * BLOCK_SECTOR_SIZE;
        ok = bytes <= mapped_size &&
             offset <= disk_size &&
             bytes <= disk_size - offset;
    }

    if (!ok) {
        return -1;
    }
    *remote_out = remote;
    *bytes_out = bytes;
    *offset_out = offset;
    return 0;
}

static void handle_read(ipc_msg_t msg) {
    char *remote = 0;
    uint64_t bytes = 0;
    uint64_t offset = 0;
    int ok = validate_io(msg, &remote, &bytes, &offset) == 0;

    if (ok) {
        if (virtio_ready) {
            ok = virtio_blk_rw(offset / BLOCK_SECTOR_SIZE, bytes, remote, 0) == 0;
        } else {
            copy_bytes(remote, disk_data + offset, bytes);
        }
    }

    uint64_t mapped_size = msg.payload[1];
    if (remote && mapped_size) {
        sys_munmap(remote, mapped_size);
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {ok ? bytes : 0};
    sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 8, reply);
}

static void handle_write(ipc_msg_t msg) {
    char *remote = 0;
    uint64_t bytes = 0;
    uint64_t offset = 0;
    int ok = validate_io(msg, &remote, &bytes, &offset) == 0;

    if (ok) {
        if (virtio_ready) {
            ok = virtio_blk_rw(offset / BLOCK_SECTOR_SIZE, bytes, remote, 1) == 0;
        } else {
            copy_bytes(disk_data + offset, remote, bytes);
        }
    }

    uint64_t mapped_size = msg.payload[1];
    if (remote && mapped_size) {
        sys_munmap(remote, mapped_size);
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {ok ? bytes : 0};
    sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 8, reply);
}

void _start(void) {
    if (virtio_blk_init() < 0 && find_apps_fat() < 0) {
        sys_exit(1);
    }

    while (ns_register(BLOCK_SERVICE_NAME) < 0) {
        sys_sleep(10);
    }

    while (1) {
        ipc_msg_t msg = sys_ipc_recv(CAP_SELF);
        uint64_t op = msg.payload[4];
        if (op == BLOCK_REQ_READ) {
            handle_read(msg);
        } else if (op == BLOCK_REQ_WRITE) {
            handle_write(msg);
        } else {
            uint64_t reply[IPC_REPLY_INLINE_WORDS] = {0};
            sys_ipc_reply(msg.reply_cap, -1, 0, 8, reply);
        }
    }
}
