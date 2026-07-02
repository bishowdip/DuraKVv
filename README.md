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

Known gap, on purpose: mutations write pages in place, so a crash mid-update
can tear a multi-page update. The next layer (write-ahead logging + crash
recovery) closes exactly that gap.

## Layout

```
include/  storage.h
src/      storage.c
tests/    test_storage.c
```

## Tests

| Test | Proves |
|------|--------|
| `tests/test_storage.c` | record round-trips, update/delete, multi-page growth, persistence across reopen |
