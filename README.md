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

`db_open` defaults to 64 frames with LRU; `db_open_ex` configures both.

## Layout

```
include/  storage.h wal.h recovery.h bufferpool.h replacement.h
src/      storage.c wal.c recovery.c bufferpool.c replacement.c durakv.c
tests/    test_storage.c test_wal_recovery.c
scripts/  crashtest.sh
```

## Tests

| Test | Proves |
|------|--------|
| `tests/test_storage.c` | record round-trips, update/delete, multi-page growth, persistence across reopen |
| `tests/test_wal_recovery.c` | redo rebuilds 500 keys from the WAL after `data.db` is rolled back to a pre-write snapshot (an honest power-loss model) |
| `scripts/crashtest.sh` | committed keys survive repeated `kill -9` |
