/*
 * replacement.h - page replacement policies (FIFO, LRU) behind a vtable, so
 * the pool never cares which one is running. both use a per-frame stamp:
 * FIFO stamps on load only (-> can suffer belady's anomaly), LRU re-stamps
 * on every access (stack algorithm, cant suffer it). victim = oldest stamp.
 */
#ifndef DURAKV_REPLACEMENT_H
#define DURAKV_REPLACEMENT_H

#include <stddef.h>

typedef enum {
    POLICY_FIFO = 0,
    POLICY_LRU  = 1
} PolicyKind;

typedef struct Replacer Replacer;

Replacer   *replacer_create(PolicyKind kind, size_t nframes);
void        replacer_destroy(Replacer *r);
const char *replacer_name(const Replacer *r);

/* lifecycle hooks the buffer pool calls */
void replacer_note_load(Replacer *r, size_t frame);    /* page loaded into frame */
void replacer_note_access(Replacer *r, size_t frame);  /* page referenced (a hit) */
void replacer_note_free(Replacer *r, size_t frame);    /* frame emptied */

/* Pick a victim among frames whose evictable[f] != 0. Returns 1 and sets
 * *out on success, or 0 if every frame is pinned/empty. */
int  replacer_victim(Replacer *r, const unsigned char *evictable, size_t *out);

#endif /* DURAKV_REPLACEMENT_H */
