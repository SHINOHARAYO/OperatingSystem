//Meow, motherfuckers!
#include "orange_cat.h"
#include "mmu.h"
#include "pmm.h"
#include <stddef.h>

static ocap_release_hook_t release_hook = NULL;

void ocap_set_release_hook(ocap_release_hook_t hook) {
    release_hook = hook;
}

static void release_cap(const ocap_t *cap) {
    if (!cap || cap->type == OCAP_NONE) {
        return;
    }
    if (release_hook) {
        release_hook(cap);
    }
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

int ocap_ensure(orange_cat_table_t *table, uint32_t min_capacity) {
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

int ocap_install(orange_cat_table_t *table, uint32_t type, uint32_t object_id,
                 uint64_t rights, uint64_t flags) {
    if (!table || type == OCAP_NONE || ocap_ensure(table, OCAP_INITIAL_CAPS) < 0) {
        return -1;
    }

    for (uint32_t i = 3; i < table->capacity; i++) {
        if (table->entries[i].type == OCAP_NONE) {
            table->entries[i].type = type;
            table->entries[i].object_id = object_id;
            table->entries[i].rights = rights;
            table->entries[i].flags = flags;
            return (int)i;
        }
    }

    uint32_t old_capacity = table->capacity;
    if (ocap_ensure(table, old_capacity + 1) < 0) {
        return -1;
    }

    table->entries[old_capacity].type = type;
    table->entries[old_capacity].object_id = object_id;
    table->entries[old_capacity].rights = rights;
    table->entries[old_capacity].flags = flags;
    return (int)old_capacity;
}

int ocap_install_at(orange_cat_table_t *table, uint32_t slot, uint32_t type,
                    uint32_t object_id, uint64_t rights, uint64_t flags) {
    if (!table || slot == 0 || type == OCAP_NONE ||
        ocap_ensure(table, slot + 1) < 0) {
        return -1;
    }

    release_cap(&table->entries[slot]);
    table->entries[slot].type = type;
    table->entries[slot].object_id = object_id;
    table->entries[slot].rights = rights;
    table->entries[slot].flags = flags;
    return (int)slot;
}

int ocap_read_slot(const orange_cat_table_t *table, uint32_t slot, ocap_t *out) {
    if (!table || !table->entries || !out || slot >= table->capacity) {
        return -1;
    }
    *out = table->entries[slot];
    return 0;
}

int ocap_lookup(const orange_cat_table_t *table, uint32_t slot, uint32_t type,
                uint64_t required_rights, ocap_t *out) {
    if (!table || !table->entries || slot == 0 || slot >= table->capacity) {
        return -1;
    }

    const ocap_t *cap = &table->entries[slot];
    if (cap->type != type || (cap->rights & required_rights) != required_rights) {
        return -1;
    }

    if (out) {
        *out = *cap;
    }
    return 0;
}

int ocap_copy(const orange_cat_table_t *src, orange_cat_table_t *dst,
              uint32_t src_slot, uint64_t required_rights,
              uint32_t *dst_slot) {
    if (!src || !dst || !src->entries || src_slot == 0 ||
        src_slot >= src->capacity) {
        return -1;
    }

    ocap_t cap = src->entries[src_slot];
    if (cap.type == OCAP_NONE || cap.type == OCAP_REPLY ||
        (cap.rights & required_rights) != required_rights) {
        return -1;
    }

    int slot = ocap_install(dst, cap.type, cap.object_id, cap.rights, cap.flags);
    if (slot < 0) {
        return -1;
    }
    if (dst_slot) {
        *dst_slot = (uint32_t)slot;
    }
    return 0;
}

void ocap_clear(orange_cat_table_t *table) {
    if (!table || !table->entries) {
        return;
    }

    table->entries[0].type = OCAP_NONE;
    table->entries[0].object_id = 0;
    table->entries[0].rights = 0;
    table->entries[0].flags = 0;
    for (uint32_t i = 0; i < table->capacity; i++) {
        ocap_revoke_slot(table, i);
    }
}

void ocap_release_table(orange_cat_table_t *table) {
    if (!table || !table->entries) {
        return;
    }

    ocap_clear(table);
    pmm_free_contiguous_pages(table->entries, table->pages);
    table->entries = NULL;
    table->capacity = 0;
    table->pages = 0;
}

void ocap_revoke_slot(orange_cat_table_t *table, uint32_t slot) {
    if (!table || !table->entries || slot == 0 || slot >= table->capacity) {
        return;
    }

    release_cap(&table->entries[slot]);
    table->entries[slot].type = OCAP_NONE;
    table->entries[slot].object_id = 0;
    table->entries[slot].rights = 0;
    table->entries[slot].flags = 0;
}
