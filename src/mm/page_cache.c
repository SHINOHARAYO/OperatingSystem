#include "page_cache.h"
#include "frame.h"
#include "initrd.h"
#include "mmu.h"
#include "log.h"
#include "spinlock.h"
#include "cache.h"

#define MAX_INITRD_CACHE_PAGES 128

typedef struct {
    int present;
    uint32_t file_index;
    uint64_t page_offset;
    uint64_t paddr;
    uint64_t last_used;
} page_cache_entry_t;

static page_cache_entry_t initrd_cache[MAX_INITRD_CACHE_PAGES];
static uint64_t page_cache_clock = 1;
static spinlock_t page_cache_lock;

int page_cache_init(void) {
    page_cache_lock.value = 0;
    uint64_t irq_flags = spin_lock_irqsave(&page_cache_lock);
    for (uint32_t i = 0; i < MAX_INITRD_CACHE_PAGES; i++) {
        initrd_cache[i].present = 0;
        initrd_cache[i].file_index = 0;
        initrd_cache[i].page_offset = 0;
        initrd_cache[i].paddr = 0;
        initrd_cache[i].last_used = 0;
    }

    page_cache_clock = 1;
    spin_unlock_irqrestore(&page_cache_lock, irq_flags);
    LOG_OK_HEX("PAGECACHE: boot archive cache pages: ", MAX_INITRD_CACHE_PAGES);
    return 0;
}

int page_cache_get_initrd_page(uint32_t file_index, uint64_t file_offset,
                               uint64_t *out_paddr) {
    if (!out_paddr) {
        return -1;
    }

    uint64_t irq_flags = spin_lock_irqsave(&page_cache_lock);
    uint64_t page_offset = file_offset & ~(PAGE_SIZE - 1);
    for (uint32_t i = 0; i < MAX_INITRD_CACHE_PAGES; i++) {
        page_cache_entry_t *entry = &initrd_cache[i];
        if (entry->present && entry->file_index == file_index &&
            entry->page_offset == page_offset) {
            if (frame_ref(entry->paddr) < 0) {
                spin_unlock_irqrestore(&page_cache_lock, irq_flags);
                return -1;
            }
            entry->last_used = page_cache_clock++;
            *out_paddr = entry->paddr;
            spin_unlock_irqrestore(&page_cache_lock, irq_flags);
            return 0;
        }
    }

    int free_slot = -1;
    for (uint32_t i = 0; i < MAX_INITRD_CACHE_PAGES; i++) {
        if (!initrd_cache[i].present) {
            free_slot = (int)i;
            break;
        }
    }

    if (free_slot < 0) {
        spin_unlock_irqrestore(&page_cache_lock, irq_flags);
        return -1;
    }

    const uint8_t *file_data = 0;
    uint64_t file_size = 0;
    if (initrd_get_file(file_index, &file_data, &file_size) < 0 ||
        page_offset >= file_size) {
        spin_unlock_irqrestore(&page_cache_lock, irq_flags);
        return -1;
    }

    frame_t *frame = frame_alloc(0, FRAME_FLAG_USER | FRAME_FLAG_PINNED);
    if (!frame) {
        spin_unlock_irqrestore(&page_cache_lock, irq_flags);
        return -1;
    }

    uint8_t *dst = (uint8_t *)frame->paddr;
    for (uint64_t i = 0; i < PAGE_SIZE; i++) {
        dst[i] = 0;
    }

    uint64_t copy_len = file_size - page_offset;
    if (copy_len > PAGE_SIZE) {
        copy_len = PAGE_SIZE;
    }

    const uint8_t *src = file_data + page_offset;
    for (uint64_t i = 0; i < copy_len; i++) {
        dst[i] = src[i];
    }
    arch_sync_icache(dst, PAGE_SIZE);

    page_cache_entry_t *entry = &initrd_cache[free_slot];
    entry->present = 1;
    entry->file_index = file_index;
    entry->page_offset = page_offset;
    entry->paddr = frame->paddr;
    entry->last_used = page_cache_clock++;

    if (frame_ref(frame->paddr) < 0) {
        entry->present = 0;
        entry->paddr = 0;
        frame_unref(frame->paddr);
        spin_unlock_irqrestore(&page_cache_lock, irq_flags);
        return -1;
    }

    *out_paddr = frame->paddr;
    spin_unlock_irqrestore(&page_cache_lock, irq_flags);
    return 0;
}
