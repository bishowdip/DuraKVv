/*
 * bufferpool.c - page cache, the software version of an OS page cache.
 * bp_pin resolves page id -> frame: resident = HIT, else FAULT (load from
 * disk, evicting a victim if full). write-back not write-through: dirty
 * frames only flush on eviction/flush_all, so kill -9 genuinely loses them
 * and redo gets tested for real. pin count > 0 = not evictable. one mutex
 * over the whole table. io hook (bp_set_io) so every transfer goes through
 * storage's page_read/page_write = the encryption choke point.
 */
#define _POSIX_C_SOURCE 200809L
#include "bufferpool.h"
#include "storage.h"        /* PAGE_SIZE, page_init */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/* One cache frame: metadata plus the page's bytes. */
typedef struct {
    uint64_t page_id;
    int      valid;     /* frame holds a page                       */
    int      dirty;     /* page modified since load (needs writeback)*/
    int      pin;       /* pin count (>0 => in use, not evictable)  */
    uint8_t  data[PAGE_SIZE];
} Frame;

struct BufferPool {
    int        fd;         /* backing file (raw path)                       */
    size_t     nframes;    /* fixed number of frames = the "memory" size    */
    Frame     *frames;
    Replacer  *repl;       /* eviction policy (FIFO/LRU), see replacement.c */
    BPStats    st;         /* accesses/hits/faults/evictions/writebacks     */
    pthread_mutex_t mtx;   /* protects frame table, replacer AND stats      */

    void        *io_ctx;   /* optional caller-supplied page I/O (else use fd) */
    bp_read_fn   io_read;  /* encryption-aware read hook, or NULL             */
    bp_write_fn  io_write; /* encryption-aware write hook, or NULL            */
};

BufferPool *bp_create(int fd, size_t nframes, PolicyKind policy)
{
    if (nframes == 0) nframes = 1;
    BufferPool *bp = calloc(1, sizeof(*bp));
    bp->fd      = fd;
    bp->nframes = nframes;
    bp->frames  = calloc(nframes, sizeof(Frame));
    bp->repl    = replacer_create(policy, nframes);
    pthread_mutex_init(&bp->mtx, NULL);
    return bp;
}

void bp_destroy(BufferPool *bp)
{
    if (!bp) return;
    pthread_mutex_destroy(&bp->mtx);
    replacer_destroy(bp->repl);
    free(bp->frames);
    free(bp);
}

/* disk -> frame. past-EOF pages come back empty. io hook wins if installed. */
static void load_from_disk(BufferPool *bp, uint64_t page_id, uint8_t *dst)
{
    if (bp->io_read) { bp->io_read(bp->io_ctx, page_id, dst); return; }
    ssize_t n = pread(bp->fd, dst, PAGE_SIZE, (off_t)page_id * PAGE_SIZE);
    if (n < (ssize_t)PAGE_SIZE) page_init(dst, page_id);
}

/* flush one frame if dirty, then clear the bit so re-flushing is free. */
static void writeback(BufferPool *bp, Frame *f)
{
    if (!f->valid || !f->dirty) return;
    if (bp->io_write) {
        bp->io_write(bp->io_ctx, f->page_id, f->data);
    } else {
        ssize_t n = pwrite(bp->fd, f->data, PAGE_SIZE, (off_t)f->page_id * PAGE_SIZE);
        if (n != (ssize_t)PAGE_SIZE) perror("bufferpool writeback");
    }
    f->dirty = 0;
    bp->st.writebacks++;
}

/* Install the encryption-aware page I/O callbacks (the single choke point that
 * keeps sealing/unsealing out of the pool and the storage engine). */
void bp_set_io(BufferPool *bp, void *ctx, bp_read_fn rd, bp_write_fn wr)
{
    bp->io_ctx = ctx; bp->io_read = rd; bp->io_write = wr;
}

/* Linear scan for a resident copy of `page_id`. O(nframes); fine for the small
 * pools used here. A production system would index this with a hash map. */
static Frame *find_resident(BufferPool *bp, uint64_t page_id, size_t *idx)
{
    for (size_t i = 0; i < bp->nframes; i++)
        if (bp->frames[i].valid && bp->frames[i].page_id == page_id) {
            if (idx) *idx = i;
            return &bp->frames[i];
        }
    return NULL;
}

/* the core. NULL only if every frame is pinned (no victim available). */
uint8_t *bp_pin(BufferPool *bp, uint64_t page_id)
{
    pthread_mutex_lock(&bp->mtx);
    bp->st.accesses++;

    size_t idx;
    Frame *f = find_resident(bp, page_id, &idx);
    if (f) {                                   /* ---- HIT: page already cached ---- */
        bp->st.hits++;
        f->pin++;
        replacer_note_access(bp->repl, idx);   /* LRU counts this as a recent use */
        pthread_mutex_unlock(&bp->mtx);
        return f->data;
    }

    /* ---- PAGE FAULT: the page is not resident, we must find a frame ---- */
    bp->st.faults++;

    /* Prefer an empty frame (the pool is still warming up). */
    size_t target = bp->nframes;
    for (size_t i = 0; i < bp->nframes; i++)
        if (!bp->frames[i].valid) { target = i; break; }

    if (target == bp->nframes) {               /* pool full -> must EVICT */
        /* Build the evictable set: resident AND unpinned. The replacement
         * policy then picks the victim among these (oldest/least-recent). */
        unsigned char *evictable = malloc(bp->nframes);
        for (size_t i = 0; i < bp->nframes; i++)
            evictable[i] = (bp->frames[i].valid && bp->frames[i].pin == 0);
        size_t victim;
        int ok = replacer_victim(bp->repl, evictable, &victim);
        free(evictable);
        if (!ok) { pthread_mutex_unlock(&bp->mtx); return NULL; }  /* all pinned */

        writeback(bp, &bp->frames[victim]);    /* persist victim if dirty */
        replacer_note_free(bp->repl, victim);
        bp->frames[victim].valid = 0;
        bp->st.evictions++;
        target = victim;
    }

    /* Load the requested page into the chosen frame and pin it. */
    Frame *nf = &bp->frames[target];
    load_from_disk(bp, page_id, nf->data);
    nf->page_id = page_id;
    nf->valid   = 1;
    nf->dirty   = 0;
    nf->pin     = 1;
    replacer_note_load(bp->repl, target);
    pthread_mutex_unlock(&bp->mtx);
    return nf->data;
}

/* Release a pin. Pass dirty=1 if the caller modified the page, so it will be
 * written back before eviction (write-back). The page only becomes eligible for
 * eviction once its pin count returns to 0. */
void bp_unpin(BufferPool *bp, uint64_t page_id, int dirty)
{
    pthread_mutex_lock(&bp->mtx);
    Frame *f = find_resident(bp, page_id, NULL);
    if (f) {
        if (dirty) f->dirty = 1;               /* sticky: never clear on unpin */
        if (f->pin > 0) f->pin--;
    }
    pthread_mutex_unlock(&bp->mtx);
}

/* Write every dirty frame to disk (used at checkpoint/close). */
void bp_flush_all(BufferPool *bp)
{
    pthread_mutex_lock(&bp->mtx);
    for (size_t i = 0; i < bp->nframes; i++)
        writeback(bp, &bp->frames[i]);
    pthread_mutex_unlock(&bp->mtx);
}

/* counters under the lock (reading them without it would be a data race;
 * const gets cast away because locking mutates the mutex). */
BPStats bp_stats(const BufferPool *bp)
{
    BufferPool *m = (BufferPool *)bp;
    pthread_mutex_lock(&m->mtx);
    BPStats s = bp->st;
    pthread_mutex_unlock(&m->mtx);
    return s;
}

double bp_hit_ratio(const BufferPool *bp)
{
    BPStats s = bp_stats(bp);
    return s.accesses ? (double)s.hits / (double)s.accesses : 0.0;
}

void bp_reset_stats(BufferPool *bp)
{
    pthread_mutex_lock(&bp->mtx);
    memset(&bp->st, 0, sizeof(bp->st));
    pthread_mutex_unlock(&bp->mtx);
}

const char *bp_policy_name(const BufferPool *bp) { return replacer_name(bp->repl); }
size_t      bp_nframes(const BufferPool *bp)     { return bp->nframes; }
