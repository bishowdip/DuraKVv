/*
 * bufferpool.c -- an in-RAM page cache (Task 2: virtual-memory/paging).
 *
 * The buffer pool sits between the store and the disk file. A fixed number of
 * `nframes` frames each hold one PAGE_SIZE page. Callers ask for a page by id
 * via bp_pin(); if it is already resident that is a HIT, otherwise it is a page
 * FAULT and the pool loads it from disk -- evicting another page first if every
 * frame is occupied. This is the software analogue of an OS/MMU page cache, and
 * the hit/miss counters here are what test_bufferpool reports.
 *
 * Two design choices carry the durability and memory stories:
 *
 *  1. WRITE-BACK, not write-through. A modified (dirty) page is NOT written to
 *     disk immediately; it is flushed only on eviction, bp_flush_all(), or
 *     close. This is faster (many updates to a hot page cost one disk write)
 *     and, crucially, means dirty pages live only in this process's RAM -- so a
 *     kill -9 genuinely loses them, which is what makes the WAL redo pass a real
 *     recovery test rather than one masked by the OS page cache.
 *
 *  2. A pluggable I/O hook (bp_set_io). By default the pool reads/writes raw
 *     pages through `fd`. The storage engine installs callbacks so that every
 *     page passes through one encryption choke point (encryption-at-rest),
 *     keeping storage.c/recovery.c free of crypto code.
 *
 * PINNING prevents eviction of a page currently in use: bp_pin increments a pin
 * count and returns the frame's buffer; the page cannot be chosen as a victim
 * until the matching bp_unpin drops the count to 0. One mutex serialises the
 * whole table so concurrent threads can fault pages safely. See
 * include/bufferpool.h.
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
