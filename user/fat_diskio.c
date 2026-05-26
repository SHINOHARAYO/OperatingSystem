#include "fat_diskio.h"
#include "block_proto.h"
#include "ff.h"
#include "diskio.h"
#include "ipc_proto.h"
#include "ns_proto.h"

static int block_cap = -1;
static char *bounce;
static int bounce_cap = -1;

static void copy_bytes_local(void *dst, const void *src, uint64_t size) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

int fat_disk_init(void) {
    if (block_cap < 0) {
        for (uint32_t attempt = 0; attempt < 50 && block_cap < 0; attempt++) {
            block_cap = ns_resolve(BLOCK_SERVICE_NAME);
            if (block_cap < 0) {
                sys_sleep(10);
            }
        }
    }
    if (block_cap < 0) {
        return -1;
    }

    if (!bounce) {
        bounce = (char *)sys_mmap(4096);
        if (!bounce) {
            return -1;
        }
        for (uint32_t i = 0; i < 4096; i += BLOCK_SECTOR_SIZE) {
            bounce[i] = 0;
        }
    }

    if (bounce_cap < 0) {
        bounce_cap = sys_mem_export(bounce, 4096,
                                    MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE);
        if (bounce_cap < 0) {
            return -1;
        }
    }

    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return pdrv == 0 && fat_disk_init() == 0 ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && fat_disk_init() == 0 ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !buff || count == 0 || fat_disk_init() < 0) {
        return RES_PARERR;
    }

    UINT done = 0;
    while (done < count) {
        UINT chunk = count - done;
        if (chunk > 8) {
            chunk = 8;
        }

        uint64_t payload[IPC_INLINE_WORDS] = {0};
        payload[0] = (uint64_t)bounce_cap;
        payload[1] = 0;
        payload[2] = 4096;
        payload[3] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE;
        payload[4] = BLOCK_REQ_READ;
        payload[5] = (uint64_t)sector + done;
        payload[6] = chunk;

        ipc_msg_t reply = sys_ipc_call((uint32_t)block_cap, IPC_FLAG_MEM,
                                       7 * sizeof(uint64_t), payload);
        if (reply.status < 0 ||
            reply.payload[0] != (uint64_t)chunk * BLOCK_SECTOR_SIZE) {
            return RES_ERROR;
        }

        copy_bytes_local(buff + ((uint64_t)done * BLOCK_SECTOR_SIZE),
                         bounce,
                         (uint64_t)chunk * BLOCK_SECTOR_SIZE);
        done += chunk;
    }

    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !buff || count == 0 || fat_disk_init() < 0) {
        return RES_PARERR;
    }

    UINT done = 0;
    while (done < count) {
        UINT chunk = count - done;
        if (chunk > 8) {
            chunk = 8;
        }

        uint64_t bytes = (uint64_t)chunk * BLOCK_SECTOR_SIZE;
        copy_bytes_local(bounce, buff + ((uint64_t)done * BLOCK_SECTOR_SIZE), bytes);

        uint64_t payload[IPC_INLINE_WORDS] = {0};
        payload[0] = (uint64_t)bounce_cap;
        payload[1] = 0;
        payload[2] = 4096;
        payload[3] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE;
        payload[4] = BLOCK_REQ_WRITE;
        payload[5] = (uint64_t)sector + done;
        payload[6] = chunk;

        ipc_msg_t reply = sys_ipc_call((uint32_t)block_cap, IPC_FLAG_MEM,
                                       7 * sizeof(uint64_t), payload);
        if (reply.status < 0 || reply.payload[0] != bytes) {
            return RES_ERROR;
        }

        done += chunk;
    }

    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || fat_disk_init() < 0) {
        return RES_PARERR;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        if (!buff) return RES_PARERR;
        *(LBA_t *)buff = 8192;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!buff) return RES_PARERR;
        *(WORD *)buff = (WORD)BLOCK_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buff) return RES_PARERR;
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
