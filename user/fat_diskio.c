#include "fat_diskio.h"
#include "ff.h"
#include "diskio.h"

#define FAT_SECTOR_SIZE 512ULL

static const unsigned char *fat_image;
static uint64_t fat_image_size;

static void copy_bytes_local(void *dst, const void *src, uint64_t size) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

void fat_disk_set_image(const void *data, uint64_t size) {
    fat_image = (const unsigned char *)data;
    fat_image_size = size;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0 || !fat_image || fat_image_size < FAT_SECTOR_SIZE) {
        return STA_NOINIT;
    }
    return 0;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0 || !fat_image || fat_image_size < FAT_SECTOR_SIZE) {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !buff || !fat_image || count == 0) {
        return RES_PARERR;
    }

    uint64_t offset = (uint64_t)sector * FAT_SECTOR_SIZE;
    uint64_t bytes = (uint64_t)count * FAT_SECTOR_SIZE;
    if (offset > fat_image_size || bytes > fat_image_size - offset) {
        return RES_PARERR;
    }

    copy_bytes_local(buff, fat_image + offset, bytes);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    (void)buff;
    (void)sector;
    (void)count;
    return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || !fat_image) {
        return RES_PARERR;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        if (!buff) return RES_PARERR;
        *(LBA_t *)buff = (LBA_t)(fat_image_size / FAT_SECTOR_SIZE);
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!buff) return RES_PARERR;
        *(WORD *)buff = (WORD)FAT_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buff) return RES_PARERR;
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
