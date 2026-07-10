/*
 * replacement.c - FIFO and LRU with ONE mechanism: a logical clock + a
 * per-frame stamp, victim = smallest stamp. only difference is when we
 * re-stamp: FIFO on load only, LRU on load AND every hit. LRU is a stack
 * algorithm (more frames never = more faults), FIFO isnt -- thats exactly
 * why FIFO can hit belady's anomaly (see tests/test_belady.c).
 */
#include "replacement.h"

#include <stdint.h>
#include <stdlib.h>

/* Per-policy behaviour. Only the "when do we re-stamp?" hooks differ. */
typedef struct Policy {
    const char *name;
    void (*note_load)  (Replacer *, size_t);   /* page loaded into frame */
    void (*note_access)(Replacer *, size_t);   /* page referenced (a hit) */
} Policy;

struct Replacer {
    const Policy *pol;        /* selected policy vtable                    */
    size_t        n;          /* number of frames tracked                 */
    uint64_t      clock;      /* logical time, advanced on each stamp     */
    uint64_t     *stamp;      /* per-frame stamp; 0 means frame is empty  */
};

/* ---- FIFO: stamp on load only; accesses do not change eviction order --- */
static void fifo_load(Replacer *r, size_t f)   { r->stamp[f] = ++r->clock; }
static void fifo_access(Replacer *r, size_t f) { (void)r; (void)f; }  /* no-op */

/* ---- LRU: stamp on load AND on every access, so a hit "refreshes" age -- */
static void lru_load(Replacer *r, size_t f)    { r->stamp[f] = ++r->clock; }
static void lru_access(Replacer *r, size_t f)  { r->stamp[f] = ++r->clock; }

/* Designated initialisers index by PolicyKind, so POLICIES[kind] is the vtable. */
static const Policy POLICIES[] = {
    [POLICY_FIFO] = { "FIFO", fifo_load, fifo_access },
    [POLICY_LRU]  = { "LRU",  lru_load,  lru_access  },
};

/* Create a replacer for `nframes` frames using the chosen policy. All stamps
 * start at 0 (calloc), i.e. every frame begins "empty". */
Replacer *replacer_create(PolicyKind kind, size_t nframes)
{
    Replacer *r = calloc(1, sizeof(*r));
    r->pol   = &POLICIES[kind];
    r->n     = nframes;
    r->clock = 0;
    r->stamp = calloc(nframes, sizeof(uint64_t));
    return r;
}

void replacer_destroy(Replacer *r)
{
    if (!r) return;
    free(r->stamp);
    free(r);
}

const char *replacer_name(const Replacer *r) { return r->pol->name; }

/* Events the buffer pool reports; each is dispatched through the policy vtable
 * so FIFO and LRU react differently to the same event stream. */
void replacer_note_load  (Replacer *r, size_t f) { r->pol->note_load(r, f); }
void replacer_note_access(Replacer *r, size_t f) { r->pol->note_access(r, f); }
/* Frame emptied (page removed): stamp 0 excludes it from victim selection. */
void replacer_note_free  (Replacer *r, size_t f) { r->stamp[f] = 0; }

/* victim = evictable frame with the oldest stamp. */
int replacer_victim(Replacer *r, const unsigned char *evictable, size_t *out)
{
    int found = 0;
    uint64_t best = 0;
    for (size_t f = 0; f < r->n; f++) {
        if (!evictable[f] || r->stamp[f] == 0) continue;
        if (!found || r->stamp[f] < best) { best = r->stamp[f]; *out = f; found = 1; }
    }
    return found;
}
