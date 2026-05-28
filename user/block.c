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

static uint64_t disk_size;
static int virtio_ready;
static uint32_t virtio_transport;
static volatile uint8_t *virtio_mmio = (volatile uint8_t *)BLOCK_VIRTIO_MMIO_VA;
static volatile uint8_t *pci_bar;
static volatile uint8_t *pci_common;
static volatile uint8_t *pci_notify;
static volatile uint8_t *pci_notify_base;
static volatile uint8_t *pci_isr;
static volatile uint8_t *pci_device;
static uint32_t pci_notify_multiplier;
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

static uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(virtio_mmio + off);
}

static void mmio_write32(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(virtio_mmio + off) = value;
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

static uint64_t pci_read64(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint64_t *)(base + off);
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

static void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static void zero_bytes(void *ptr, uint64_t size) {
    unsigned char *p = (unsigned char *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
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

    page[0] = 0;
    if (dma_export_range(page, 4096, out_pa, 0) < 0) {
        sys_munmap(page, 4096);
        return 0;
    }

    zero_bytes(page, 4096);
    return page;
}

static void *dma_pages(uint64_t page_count, uint64_t *out_pa) {
    unsigned char *mem = (unsigned char *)sys_mmap(page_count * 4096);
    if (!mem) {
        return 0;
    }

    uint64_t base_pa = 0;
    uint32_t dma_cap = 0;
    if (dma_export_range(mem, page_count * 4096, &base_pa, &dma_cap) < 0) {
        sys_munmap(mem, page_count * 4096);
        return 0;
    }

    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t pa = sys_dma_paddr(dma_cap, i * 4096);
        if (!pa || (i > 0 && pa != base_pa + (i * 4096))) {
            sys_dma_release(dma_cap);
            sys_munmap(mem, page_count * 4096);
            return 0;
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

static int setup_pci_queue(void) {
    pci_write16(pci_common, VIRTIO_PCI_COMMON_Q_SELECT, 0);
    uint16_t qmax = pci_read16(pci_common, VIRTIO_PCI_COMMON_Q_SIZE);
    if (qmax < VIRTQ_SIZE) {
        return -1;
    }

    pci_write16(pci_common, VIRTIO_PCI_COMMON_Q_SIZE, VIRTQ_SIZE);
    uint16_t notify_off = pci_read16(pci_common, VIRTIO_PCI_COMMON_Q_NOTIFY_OFF);
    pci_notify = pci_notify_base + ((uint64_t)notify_off * pci_notify_multiplier);
    pci_write64(pci_common, VIRTIO_PCI_COMMON_Q_DESC, vq_desc_pa);
    pci_write64(pci_common, VIRTIO_PCI_COMMON_Q_DRIVER, vq_avail_pa);
    pci_write64(pci_common, VIRTIO_PCI_COMMON_Q_DEVICE, vq_used_pa);
    pci_write16(pci_common, VIRTIO_PCI_COMMON_Q_ENABLE, 1);
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
    block_boot_info_t *boot = (block_boot_info_t *)BLOCK_BOOT_INFO_VA;
    if (boot->magic == BLOCK_BOOT_MAGIC &&
        boot->version == BLOCK_BOOT_VERSION &&
        boot->transport == BLOCK_TRANSPORT_PCI_MODERN) {
        virtio_transport = BLOCK_TRANSPORT_PCI_MODERN;
        pci_bar = (volatile uint8_t *)boot->bar_va;
        pci_common = pci_bar + boot->common_off;
        pci_notify_base = pci_bar + boot->notify_off;
        pci_isr = pci_bar + boot->isr_off;
        pci_device = pci_bar + boot->device_off;
        pci_notify_multiplier = boot->notify_multiplier;

        pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, 0);
        barrier();
        pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
        pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS,
                   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

        uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
        pci_write32(pci_common, VIRTIO_PCI_COMMON_DFSELECT, 1);
        uint32_t features_hi = pci_read32(pci_common, VIRTIO_PCI_COMMON_DF);
        if (!(features_hi & (1U << VIRTIO_F_VERSION_1_LOW_BIT))) {
            pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }

        pci_write32(pci_common, VIRTIO_PCI_COMMON_GFSELECT, 0);
        pci_write32(pci_common, VIRTIO_PCI_COMMON_GF, 0);
        pci_write32(pci_common, VIRTIO_PCI_COMMON_GFSELECT, 1);
        pci_write32(pci_common, VIRTIO_PCI_COMMON_GF, 1U << VIRTIO_F_VERSION_1_LOW_BIT);

        status |= VIRTIO_STATUS_FEATURES_OK;
        pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, status);
        if (!(pci_read8(pci_common, VIRTIO_PCI_COMMON_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }

        vq_desc = (virtq_desc_t *)dma_page(&vq_desc_pa);
        vq_avail = (virtq_avail_t *)dma_page(&vq_avail_pa);
        vq_used = (virtq_used_t *)dma_page(&vq_used_pa);
        vq_req = (virtio_blk_req_t *)dma_page(&vq_req_pa);
        if (!vq_desc || !vq_avail || !vq_used || !vq_req || setup_pci_queue() < 0) {
            pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }
        vq_status = (unsigned char *)vq_req + 32;

        disk_size = pci_read64(pci_device, 0) * BLOCK_SECTOR_SIZE;
        if (disk_size == 0) {
            pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
            return -1;
        }

        status |= VIRTIO_STATUS_DRIVER_OK;
        pci_write8(pci_common, VIRTIO_PCI_COMMON_STATUS, status);
        barrier();
        virtio_ready = 1;
        return 0;
    }

    virtio_transport = BLOCK_TRANSPORT_MMIO;
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
    uint64_t export_size = page_align_size(bytes);
    uint32_t data_cap = 0;
    uint64_t data_pa = 0;
    if (dma_export_range(remote, export_size, &data_pa, &data_cap) < 0) {
        return -1;
    }
    if (!virtio_ready || !data_pa || bytes == 0 || (bytes & (BLOCK_SECTOR_SIZE - 1)) != 0) {
        sys_dma_release(data_cap);
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
    if (virtio_transport == BLOCK_TRANSPORT_PCI_MODERN) {
        pci_write16(pci_notify, 0, 0);
    } else {
        mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    }

    for (uint64_t spins = 0; spins < 10000000; spins++) {
        barrier();
        if (vq_used->idx != vq_used_idx) {
            vq_used_idx = vq_used->idx;
            if (virtio_transport == BLOCK_TRANSPORT_PCI_MODERN) {
                (void)pci_read8(pci_isr, 0);
            } else {
                uint32_t irq_status = mmio_read32(VIRTIO_MMIO_IRQ_STATUS);
                if (irq_status) {
                    mmio_write32(VIRTIO_MMIO_IRQ_ACK, irq_status);
                }
            }
            int ok = *vq_status == VIRTIO_BLK_S_OK ? 0 : -1;
            sys_dma_release(data_cap);
            return ok;
        }
    }

    sys_dma_release(data_cap);
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
        ok = virtio_blk_rw(offset / BLOCK_SECTOR_SIZE, bytes, remote, 0) == 0;
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
        ok = virtio_blk_rw(offset / BLOCK_SECTOR_SIZE, bytes, remote, 1) == 0;
    }

    uint64_t mapped_size = msg.payload[1];
    if (remote && mapped_size) {
        sys_munmap(remote, mapped_size);
    }

    uint64_t reply[IPC_REPLY_INLINE_WORDS] = {ok ? bytes : 0};
    sys_ipc_reply(msg.reply_cap, ok ? 0 : -1, 0, 8, reply);
}

void _start(void) {
    if (virtio_blk_init() < 0) {
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
