#include "frame.h"
#include "pmm.h"
#include "mmu.h"
#include "log.h"
#include "spinlock.h"

static frame_t *frames = 0;
static uint64_t frame_count = 0;
static spinlock_t frame_lock;

int frame_init(void) {
    frame_count = pmm_get_page_count();
    if (frame_count == 0) {
        return -1;
    }

    uint64_t bytes = frame_count * sizeof(frame_t);
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    frames = (frame_t *)pmm_alloc_contiguous_pages(pages);
    if (!frames) {
        LOG_FAIL("FRAME: Failed to allocate frame table.");
        frame_count = 0;
        return -1;
    }

    for (uint64_t i = 0; i < frame_count; i++) {
        frames[i].paddr = pmm_page_from_index(i);
        frames[i].refcount = 0;
        frames[i].owner_tid = 0;
        frames[i].flags = 0;
    }

    LOG_OK_HEX("FRAME: Initialized frame objects: ", frame_count);
    return 0;
}

frame_t *frame_from_paddr(uint64_t paddr) {
    if (!frames) {
        return 0;
    }

    uint64_t index = 0;
    if (pmm_page_index(paddr & ~(PAGE_SIZE - 1), &index) < 0 || index >= frame_count) {
        return 0;
    }

    return &frames[index];
}

frame_t *frame_alloc(uint32_t owner_tid, uint64_t flags) {
    void *page = pmm_alloc_page();
    if (!page) {
        return 0;
    }

    frame_t *frame = frame_from_paddr((uint64_t)page);
    if (!frame) {
        pmm_free_page(page);
        return 0;
    }

    uint64_t irq_flags = spin_lock_irqsave(&frame_lock);
    frame->paddr = (uint64_t)page;
    frame->refcount = 1;
    frame->owner_tid = owner_tid;
    frame->flags = flags;
    spin_unlock_irqrestore(&frame_lock, irq_flags);
    return frame;
}

int frame_ref(uint64_t paddr) {
    frame_t *frame = frame_from_paddr(paddr);
    uint64_t irq_flags = spin_lock_irqsave(&frame_lock);
    if (!frame || frame->refcount == 0 || frame->refcount == UINT32_MAX) {
        spin_unlock_irqrestore(&frame_lock, irq_flags);
        return -1;
    }

    frame->refcount++;
    frame->flags |= FRAME_FLAG_SHARED;
    spin_unlock_irqrestore(&frame_lock, irq_flags);
    return 0;
}

int frame_unref(uint64_t paddr) {
    frame_t *frame = frame_from_paddr(paddr);
    uint64_t irq_flags = spin_lock_irqsave(&frame_lock);
    if (!frame || frame->refcount == 0) {
        spin_unlock_irqrestore(&frame_lock, irq_flags);
        return -1;
    }

    frame->refcount--;
    if (frame->refcount == 0) {
        uint64_t page = frame->paddr;
        frame->owner_tid = 0;
        frame->flags = 0;
        spin_unlock_irqrestore(&frame_lock, irq_flags);
        pmm_free_page((void *)page);
        return 0;
    }

    spin_unlock_irqrestore(&frame_lock, irq_flags);
    return 0;
}
