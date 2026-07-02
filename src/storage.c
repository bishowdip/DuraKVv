/*
 * storage.c -- the storage engine: the durable core the whole system will be
 * built on. It owns the on-disk page file and turns it into a key/value
 * store. Two cooperating pieces live here:
 *
 *  1. SLOTTED PAGES. The file is an array of fixed-size pages. Each page has a
 *     header, a slot directory growing downward from the top, and variable-
 *     length records growing upward from the bottom; a record is addressed by
 *     (page_id, slot). This is the standard database page layout because it lets
 *     records vary in size and be deleted (tombstoned) without moving others.
 *
 *  2. KEY DIRECTORY. An in-memory chained-hash map from key -> (page_id, slot)
 *     for O(1) average lookup. It is not persisted; it is rebuilt by scanning
 *     the pages at open, so it can never disagree with disk.
 *
 * See include/storage.h for the public contract and on-disk structures.
 */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ====================================================================== */
/* Slotted-page primitives                                                */
/* ====================================================================== */

/* Initialise a blank page: zero it, then set the header. free_end marks the
 * bottom of the record area and moves DOWN as records are inserted, while the
 * slot directory grows UP from just after the header -- the page is full when
 * the two meet. */
void page_init(uint8_t *page, uint64_t page_id)
{
    memset(page, 0, PAGE_SIZE);
    PageHeader *h = (PageHeader *)page;
    h->page_id  = page_id;
    h->page_lsn = 0;
    h->n_slots  = 0;
    h->free_end = PAGE_SIZE;     /* record area is empty; grows downward */
}

uint16_t page_free_space(const uint8_t *page)
{
    const PageHeader *h = (const PageHeader *)page;
    size_t dir_end = sizeof(PageHeader) + (size_t)h->n_slots * sizeof(Slot);
    if (h->free_end <= dir_end) return 0;
    return (uint16_t)(h->free_end - dir_end);
}

/* Insert a record, returning its slot index via slot_out. Places the record at
 * the bottom of the free area and appends a directory slot at the top. Returns
 * -1 if it would not fit (record area and slot directory would collide). */
int page_insert(uint8_t *page, const uint8_t *rec, uint16_t reclen, uint16_t *slot_out)
{
    PageHeader *h = (PageHeader *)page;
    /* one more slot must fit alongside the new record */
    size_t dir_end = sizeof(PageHeader) + ((size_t)h->n_slots + 1) * sizeof(Slot);
    if (h->free_end < reclen) return -1;
    uint16_t new_off = (uint16_t)(h->free_end - reclen);
    if (new_off < dir_end) return -1;       /* would collide with slot dir */

    memcpy(page + new_off, rec, reclen);
    Slot *slots = (Slot *)(page + sizeof(PageHeader));
    slots[h->n_slots].off = new_off;
    slots[h->n_slots].len = reclen;
    *slot_out = h->n_slots;
    h->n_slots++;
    h->free_end = new_off;
    return 0;
}

/* Delete a record by marking its slot dead (len=0). This is a "tombstone": the
 * record's bytes are left in place and its space is reclaimed only when the page
 * is later compacted -- so deletes are cheap and never shift other records. */
void page_tombstone(uint8_t *page, uint16_t slot)
{
    PageHeader *h = (PageHeader *)page;
    if (slot >= h->n_slots) return;
    Slot *slots = (Slot *)(page + sizeof(PageHeader));
    slots[slot].len = 0;        /* space is reclaimed only on compaction */
}

const uint8_t *page_slot_ptr(const uint8_t *page, uint16_t slot, uint16_t *len_out)
{
    const PageHeader *h = (const PageHeader *)page;
    if (slot >= h->n_slots) return NULL;
    const Slot *slots = (const Slot *)(page + sizeof(PageHeader));
    if (slots[slot].len == 0) return NULL;
    if (len_out) *len_out = slots[slot].len;
    return page + slots[slot].off;
}

/* ====================================================================== */
/* Record framing                                                         */
/* ====================================================================== */

uint16_t record_build(uint8_t *buf, const char *key, uint16_t klen,
                      const void *val, uint32_t vlen)
{
    uint8_t *p = buf;
    memcpy(p, &klen, 2);              p += 2;
    memcpy(p, key, klen);            p += klen;
    memcpy(p, &vlen, 4);             p += 4;
    memcpy(p, val, vlen);           p += vlen;
    return (uint16_t)(p - buf);
}

void record_parse(const uint8_t *rec, const char **key, uint16_t *klen,
                  const uint8_t **val, uint32_t *vlen)
{
    uint16_t kl; uint32_t vl;
    const uint8_t *p = rec;
    memcpy(&kl, p, 2);   p += 2;
    *key  = (const char *)p;  *klen = kl;   p += kl;
    memcpy(&vl, p, 4);   p += 4;
    *val  = p;  *vlen = vl;
}
