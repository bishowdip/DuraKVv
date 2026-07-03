# DuraKVv

A key–value store in C, built in layers. The goal is a crash-safe,
multi-client store; this is the first layer — the storage engine.

## Build & test

```bash
make            # build the unit tests
make test       # run them
make clean
```

Requires only a C11 compiler (clang/gcc). Page size is compile-time
configurable: `make CFLAGS="... -DPAGE_SIZE=8192"`.

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
to redo a committed change or undo an in-flight one. Actually replaying it on
startup — crash recovery — is the next layer.

## Layout

```
include/  storage.h wal.h
src/      storage.c wal.c
tests/    test_storage.c
```

## Tests

| Test | Proves |
|------|--------|
| `tests/test_storage.c` | record round-trips, update/delete, multi-page growth, persistence across reopen |
