# DuraKVv

A key–value store in C, built in layers. The goal is a crash-safe,
multi-client store; this is the first layer — the storage engine.

## Build & run

```bash
make            # build the durakv CLI and the tests
make test       # run the unit tests
make crashtest  # the headline kill -9 durability demo
make clean
```

Requires only a C11 compiler (clang/gcc). Page size is compile-time
configurable: `make CFLAGS="... -DPAGE_SIZE=8192"`.

### CLI

```bash
./durakv data.db wal.log
```

At a terminal it shows a guided menu; with piped input it falls back to a raw
line parser so scripts and the crashtest are unaffected:

| Command | Response |
|---------|----------|
| `set <key> <value...>` | `OK` |
| `get <key>` | `VALUE <value>` / `NOTFOUND` |
| `del <key>` | `OK` / `NOTFOUND` |
| `list` | one key per line, then `END` |
| `checkpoint` | `OK` (fsync data + WAL checkpoint marker) |
| `quit` | exit |

```
$ printf 'set city kathmandu\nget city\nquit\n' | ./durakv data.db wal.log
OK
VALUE kathmandu
```

## Storage engine

The store lives in a single page file (`data.db`), an array of fixed-size
pages (4 KiB by default):

- **Page 0** is the immutable header page (magic / version / page size).
- **Pages 1..N-1** are **slotted pages**: a header, a slot directory growing
  down from the top, and variable-length `{key,value}` records growing up
  from the bottom. A record is addressed by `(page_id, slot)`; deletes
  tombstone the slot without moving other records.
- An in-memory **key directory** (chained hash map, `key -> (page_id, slot)`)
  gives O(1) average lookup. It is never persisted — it is rebuilt by
  scanning the pages at open, so it can never disagree with disk.

## Durability (write-ahead logging)

Every mutation goes through the write-ahead log (`src/wal.c`) before it touches
`data.db`:

1. The changed pages are gathered and logged as `BEGIN` / `UPDATE...` /
   `COMMIT` records, appended to `wal.log`. Each record is length-prefixed and
   carries a **CRC32** plus the full **before- and after-image** of the page.
2. On `COMMIT` the WAL is `fsync`'d (macOS: `F_FULLFSYNC`) **before** the data
   pages are written back — so an acknowledged write is already on stable
   storage, and the data file is always allowed to lag the log.
3. A crash mid-append leaves a torn tail; the length prefix and CRC make it
   detectable, so the scanner (`wal_scan`) stops at the first bad record and
   never replays a half-written one.

This closes the tearing gap the storage layer left open: the log holds enough
to redo a committed change or undo an in-flight one.

## Crash recovery

On every open, `recovery_run` (`src/recovery.c`) replays the WAL in three
passes — a simplified ARIES:

1. **Analysis** — scan forward from the last `CHECKPOINT` to find which
   transactions committed (winners) and which were in flight (losers), plus
   the highest LSN/txn ids seen (used to resume the counters).
2. **Redo** — re-apply every committed after-image, but only where
   `page.page_lsn < record.lsn`. That comparison makes redo **idempotent**, so
   a crash *during* recovery is itself harmless — it just re-runs.
3. **Undo** — restore the before-image of every uncommitted update, so a
   partially-applied loser leaves no trace (atomicity).

Because each record holds a full page image, redo/undo also transparently
repair a torn (half-written) page.

```
$ make crashtest
iter  1: acked +25   total=25     missing=0
...
crashtest: PASS -- 2142 committed keys survived 25 kill -9 cycles
```

## Buffer pool (Phase 2)

The live read/write path goes through an in-RAM page cache
(`src/bufferpool.c`) with pluggable replacement policies (`src/replacement.c`,
a function-pointer vtable — FIFO and LRU ship; CLOCK/LRU-K would just be more
entries). Pages are **write-back**: a modified page is only flushed to
`data.db` on eviction, checkpoint, or close.

This tightens the durability story: because dirty pages live only in this
process's RAM, a `kill -9` really does lose them, so recovery's redo pass is
exercised on every restart — not masked by the OS page cache. The pool has its
own mutex, so page faults stay safe once the store goes multi-threaded.

`db_open` defaults to 64 frames with LRU; `db_open_ex` configures both. The CLI
reads `DURAKV_FRAMES=<n>` (default 64) and `DURAKV_POLICY=fifo|lru` (default
`lru`), and its `stats` command reports the live pool counters.

Two demonstrations earn the Phase 2 marks:

- **Belady's anomaly** (`tests/test_belady.c`): the reference string
  `1 2 3 4 1 2 5 1 2 3 4 5` faults **more with 4 frames than with 3** under
  FIFO (9 → 10), while LRU is monotonic (10 → 8). Driven through the real
  buffer pool, not a model.
- **FIFO vs LRU hit ratios** (`tests/test_bufferpool.c`): a report across
  looping and skewed-locality workloads — e.g. LRU 82.8% vs FIFO 73.2% on an
  80/20 hot-set.

## Concurrency (Phase 3)

The store is multi-threaded:

- **Thread pool** (`src/threadpool.c`) — N workers over a bounded job queue
  guarded by one mutex and two condition variables (`not_empty`, `not_full`):
  the textbook producer–consumer. Every wait loops on its predicate, so
  spurious wakeups are harmless.
- **Round-robin scheduler** (`src/scheduler.c`) — per-client queues serviced
  in rotation, so a heavy client cannot starve the others (real fairness, not
  a simulation).
- **Thread-safe store** — a `pthread_rwlock_t` lets `GET` run as a shared
  reader while `SET`/`DEL` take it exclusively; the buffer pool has its own
  mutex so concurrent readers can fault pages safely. The lock nesting is
  always DB rwlock (outer) → buffer-pool mutex (inner), and the whole path is
  verified race-free under **ThreadSanitizer**.
- **Deadlock prevention** — the demo shows a strict ascending lock order
  breaking the Coffman circular-wait condition.

Demos (`make test` runs them):

| Demo | Shows |
|------|-------|
| `tests/demo_race.c` | unsynchronised `count++` loses ~140k updates; mutex and C11 `_Atomic` are exact |
| `tests/demo_deadlock.c` | naive opposite-order locking deadlocks (a watchdog kills it); ascending-order locking completes |
| `tests/demo_scheduler.c` | round-robin service order — light clients finish without waiting for a heavy client to drain |
| `tests/loadtest.c` | 16 concurrent clients × 120 ops through the thread pool, every value verified |

Throughput is **fsync-bound**: each commit does a full `F_FULLFSYNC` under the
write lock, so commits serialise at a few ms each. That is the price of
durability, paid deliberately. `make crashtest_concurrent` runs the `kill -9`
loop with 4 writer threads per batch.

## Layout

```
include/  storage.h wal.h recovery.h bufferpool.h replacement.h
          threadpool.h scheduler.h
src/      storage.c wal.c recovery.c bufferpool.c replacement.c
          threadpool.c scheduler.c durakv.c
tests/    test_storage.c test_wal_recovery.c test_bufferpool.c test_belady.c
          mem_demo.c demo_race.c demo_deadlock.c demo_scheduler.c loadtest.c
scripts/  crashtest.sh
```

## Tests

| Test | Proves |
|------|--------|
| `tests/test_storage.c` | record round-trips, update/delete, multi-page growth, persistence across reopen |
| `tests/test_wal_recovery.c` | redo rebuilds 500 keys from the WAL after `data.db` is rolled back to a pre-write snapshot (an honest power-loss model) |
| `tests/test_bufferpool.c` | page faults, eviction, dirty write-back; FIFO/LRU hit-ratio report |
| `tests/test_belady.c` | Belady's anomaly under FIFO; LRU stays monotonic |
| `tests/mem_demo.c` | a simple pointer-level paging + FIFO demonstration |
| `tests/demo_*` / `loadtest` | concurrency (see the Phase 3 table above) |
| `scripts/crashtest.sh` | committed keys survive repeated `kill -9`; `make crashtest_concurrent` does it with 4 writer threads |
