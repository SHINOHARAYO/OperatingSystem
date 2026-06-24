#include "orange_cat.h"
#include "mmu.h"
#include "pmm.h"
#include <stddef.h>

static ocap_release_hook_t release_hook = NULL;
static spinlock_t release_hook_lock;
#define OCAP_RELEASE_TYPE_COUNT 16
static ocap_release_hook_t type_release_hooks[OCAP_RELEASE_TYPE_COUNT];
static ocap_acquire_hook_t type_acquire_hooks[OCAP_RELEASE_TYPE_COUNT];

void ocap_set_release_hook(ocap_release_hook_t hook) {
    uint64_t irq_flags = spin_lock_irqsave(&release_hook_lock);
    release_hook = hook;
    spin_unlock_irqrestore(&release_hook_lock, irq_flags);
}

int ocap_set_type_release_hook(uint32_t type, ocap_release_hook_t hook) {
    if (type == OCAP_NONE || type >= OCAP_RELEASE_TYPE_COUNT) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&release_hook_lock);
    type_release_hooks[type] = hook;
    spin_unlock_irqrestore(&release_hook_lock, irq_flags);
    return 0;
}

int ocap_set_type_acquire_hook(uint32_t type, ocap_acquire_hook_t hook) {
    if (type == OCAP_NONE || type >= OCAP_RELEASE_TYPE_COUNT) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&release_hook_lock);
    type_acquire_hooks[type] = hook;
    spin_unlock_irqrestore(&release_hook_lock, irq_flags);
    return 0;
}

static void release_cap(const ocap_t *cap) {
    if (!cap || cap->type == OCAP_NONE) {
        return;
    }
    uint64_t irq_flags = spin_lock_irqsave(&release_hook_lock);
    ocap_release_hook_t hook = release_hook;
    ocap_release_hook_t type_hook = cap->type < OCAP_RELEASE_TYPE_COUNT
                                        ? type_release_hooks[cap->type]
                                        : NULL;
    spin_unlock_irqrestore(&release_hook_lock, irq_flags);
    if (type_hook) {
        type_hook(cap);
    }
    if (hook) {
        hook(cap);
    }
}

static int acquire_cap(const ocap_t *cap) {
    if (!cap || cap->type == OCAP_NONE) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&release_hook_lock);
    ocap_acquire_hook_t hook = cap->type < OCAP_RELEASE_TYPE_COUNT
                                   ? type_acquire_hooks[cap->type]
                                   : NULL;
    spin_unlock_irqrestore(&release_hook_lock, irq_flags);
    return hook ? hook(cap) : 0;
}

static void zero_bytes(void *ptr, uint64_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static uint32_t capacity_for_pages(uint32_t pages) {
    return (uint32_t)(((uint64_t)pages * PAGE_SIZE) / sizeof(ocap_t));
}

static int ocap_ensure_locked(orange_cat_table_t *table, uint32_t min_capacity) {
    if (!table) {
        return -1;
    }

    if (table->entries && table->capacity >= min_capacity) {
        return 0;
    }

    uint32_t new_pages = table->pages ? table->pages : 1;
    while (capacity_for_pages(new_pages) < min_capacity ||
           capacity_for_pages(new_pages) < OCAP_INITIAL_CAPS) {
        new_pages *= 2;
    }

    ocap_t *new_entries = (ocap_t *)pmm_alloc_contiguous_pages(new_pages);
    if (!new_entries) {
        return -1;
    }
    zero_bytes(new_entries, (uint64_t)new_pages * PAGE_SIZE);

    if (table->entries) {
        for (uint32_t i = 0; i < table->capacity; i++) {
            new_entries[i] = table->entries[i];
        }
        pmm_free_contiguous_pages(table->entries, table->pages);
    }

    table->entries = new_entries;
    table->pages = new_pages;
    table->capacity = capacity_for_pages(new_pages);
    return 0;
}

int ocap_ensure(orange_cat_table_t *table, uint32_t min_capacity) {
    if (!table) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    int result = ocap_ensure_locked(table, min_capacity);
    spin_unlock_irqrestore(&table->lock, irq_flags);
    return result;
}

int ocap_install(orange_cat_table_t *table, uint32_t type, uint32_t object_id,
                 uint64_t rights, uint64_t flags) {
    if (!table || type == OCAP_NONE) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    if (ocap_ensure_locked(table, OCAP_INITIAL_CAPS) < 0) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return -1;
    }

    for (uint32_t i = OCAP_FIRST_DYNAMIC_SLOT; i < table->capacity; i++) {
        if (table->entries[i].type == OCAP_NONE) {
            table->entries[i].type = type;
            table->entries[i].object_id = object_id;
            table->entries[i].rights = rights;
            table->entries[i].flags = flags;
            spin_unlock_irqrestore(&table->lock, irq_flags);
            return (int)i;
        }
    }

    uint32_t old_capacity = table->capacity;
    if (ocap_ensure_locked(table, old_capacity + 1) < 0) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return -1;
    }

    table->entries[old_capacity].type = type;
    table->entries[old_capacity].object_id = object_id;
    table->entries[old_capacity].rights = rights;
    table->entries[old_capacity].flags = flags;
    spin_unlock_irqrestore(&table->lock, irq_flags);
    return (int)old_capacity;
}

int ocap_install_at(orange_cat_table_t *table, uint32_t slot, uint32_t type,
                    uint32_t object_id, uint64_t rights, uint64_t flags) {
    if (!table || slot == 0 || type == OCAP_NONE) {
        return -1;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    if (ocap_ensure_locked(table, slot + 1) < 0) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return -1;
    }

    release_cap(&table->entries[slot]);
    table->entries[slot].type = type;
    table->entries[slot].object_id = object_id;
    table->entries[slot].rights = rights;
    table->entries[slot].flags = flags;
    spin_unlock_irqrestore(&table->lock, irq_flags);
    return (int)slot;
}

int ocap_read_slot(const orange_cat_table_t *table, uint32_t slot, ocap_t *out) {
    if (!table || !out) {
        return -1;
    }
    orange_cat_table_t *mutable_table = (orange_cat_table_t *)table;
    uint64_t irq_flags = spin_lock_irqsave(&mutable_table->lock);
    if (!table->entries || slot >= table->capacity) {
        spin_unlock_irqrestore(&mutable_table->lock, irq_flags);
        return -1;
    }
    *out = table->entries[slot];
    spin_unlock_irqrestore(&mutable_table->lock, irq_flags);
    return 0;
}

int ocap_lookup(const orange_cat_table_t *table, uint32_t slot, uint32_t type,
                uint64_t required_rights, ocap_t *out) {
    if (!table) {
        return -1;
    }
    orange_cat_table_t *mutable_table = (orange_cat_table_t *)table;
    uint64_t irq_flags = spin_lock_irqsave(&mutable_table->lock);
    if (!table->entries || slot == 0 || slot >= table->capacity) {
        spin_unlock_irqrestore(&mutable_table->lock, irq_flags);
        return -1;
    }

    const ocap_t *cap = &table->entries[slot];
    if (cap->type != type || (cap->rights & required_rights) != required_rights) {
        spin_unlock_irqrestore(&mutable_table->lock, irq_flags);
        return -1;
    }

    if (out) {
        *out = *cap;
    }
    spin_unlock_irqrestore(&mutable_table->lock, irq_flags);
    return 0;
}

int ocap_copy(const orange_cat_table_t *src, orange_cat_table_t *dst,
              uint32_t src_slot, uint64_t required_rights,
              uint32_t *dst_slot) {
    if (!src || !dst) {
        return -1;
    }

    ocap_t cap;
    if (ocap_read_slot(src, src_slot, &cap) < 0) {
        return -1;
    }
    if (cap.type == OCAP_NONE || cap.type == OCAP_REPLY ||
        (cap.rights & required_rights) != required_rights) {
        return -1;
    }

    if (acquire_cap(&cap) < 0) {
        return -1;
    }
    int slot = ocap_install(dst, cap.type, cap.object_id, cap.rights, cap.flags);
    if (slot < 0) {
        release_cap(&cap);
        return -1;
    }
    if (dst_slot) {
        *dst_slot = (uint32_t)slot;
    }
    return 0;
}

int ocap_clone_at(const orange_cat_table_t *src, orange_cat_table_t *dst,
                  uint32_t src_slot, uint32_t dst_slot,
                  uint64_t required_rights) {
    if (!src || !dst) {
        return -1;
    }
    ocap_t cap;
    if (ocap_read_slot(src, src_slot, &cap) < 0 ||
        cap.type == OCAP_NONE || cap.type == OCAP_REPLY ||
        (cap.rights & required_rights) != required_rights) {
        return -1;
    }
    if (acquire_cap(&cap) < 0) {
        return -1;
    }
    if (ocap_install_at(dst, dst_slot, cap.type, cap.object_id,
                        cap.rights, cap.flags) < 0) {
        release_cap(&cap);
        return -1;
    }
    return 0;
}

void ocap_clear(orange_cat_table_t *table) {
    if (!table) {
        return;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    if (!table->entries) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return;
    }
    for (uint32_t i = 1; i < table->capacity; i++) {
        release_cap(&table->entries[i]);
        zero_bytes(&table->entries[i], sizeof(ocap_t));
    }
    zero_bytes(&table->entries[0], sizeof(ocap_t));
    spin_unlock_irqrestore(&table->lock, irq_flags);
}

void ocap_release_table(orange_cat_table_t *table) {
    if (!table) {
        return;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    if (!table->entries) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return;
    }
    for (uint32_t i = 1; i < table->capacity; i++) {
        release_cap(&table->entries[i]);
    }
    ocap_t *entries = table->entries;
    uint32_t pages = table->pages;
    table->entries = NULL;
    table->capacity = 0;
    table->pages = 0;
    spin_unlock_irqrestore(&table->lock, irq_flags);
    pmm_free_contiguous_pages(entries, pages);
}

void ocap_revoke_slot(orange_cat_table_t *table, uint32_t slot) {
    if (!table) {
        return;
    }
    uint64_t irq_flags = spin_lock_irqsave(&table->lock);
    if (!table->entries || slot == 0 || slot >= table->capacity) {
        spin_unlock_irqrestore(&table->lock, irq_flags);
        return;
    }
    release_cap(&table->entries[slot]);
    table->entries[slot].type = OCAP_NONE;
    table->entries[slot].object_id = 0;
    table->entries[slot].rights = 0;
    table->entries[slot].flags = 0;
    spin_unlock_irqrestore(&table->lock, irq_flags);
}
