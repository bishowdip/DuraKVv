# DuraKVv

A crash-safe, multi-client key–value store in C, built in layers: the durable
spine (storage + write-ahead log + crash recovery), a buffer pool (paging with
FIFO/LRU eviction), concurrency (thread pool, round-robin scheduler, a
thread-safe store), network/IPC (an AF_UNIX client/server), and a security
layer (encryption, authentication, permissions). The core engine needs only
**pthreads** (in libc); **libsodium** is confined to the security demos.

## Build & run

```bash
make            # build the durakv CLI, server, client and web bridge
make test       # run all unit tests and demos
make demo       # guided showcase of every feature, grouped by task
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

## Network & IPC (Phase 4)

Clients reach the store over a **Unix domain socket** (`AF_UNIX`) rather than
TCP/IP — the right tool for local inter-process communication (no network
stack, access controlled by filesystem permissions, lower overhead). The
server (`src/server.c`) accepts connections and hands each to the thread pool,
so many clients are served at once; messages use length-prefixed framing
(`src/protocol.c`).

```bash
./durakv-server /tmp/durakv.sock data.db wal.log 4   # 4 worker threads
./durakv-client /tmp/durakv.sock                     # friendly guided client
```

Wire commands: `PING`, `SET <key> <value>`, `GET <key>`, `DEL <key>`,
`STATS`, `QUIT`. Each message is a length-prefixed frame
(`[4-byte big-endian length][payload]`).

```
$ printf 'SET city kathmandu\nGET city\nQUIT\n' | ./durakv-client /tmp/durakv.sock
OK
OK kathmandu
BYE
```

For pure **message passing**, `tests/demo_mqueue.c` shows a System V message
queue shared between a parent and child process, including type-selective
receive (pull a priority message ahead of FIFO ones) — something a byte stream
cannot do. (System V IPC is used because macOS does not implement POSIX
`mq_*`.)

### Web dashboard (no terminal needed)

For a click-driven demo anyone can run, `durakv-web` starts a small **pure-C
HTTP bridge** and opens a browser control room:

```bash
make                 # builds durakv-web too
./durakv-web         # then open http://127.0.0.1:8080
```

Browsers speak HTTP/TCP; the graded server speaks AF_UNIX. So `durakv-web`
supervises a **real `durakv-server` child** and relays every dashboard action
to it over the AF_UNIX protocol — nothing on the page is faked. It provides a
key/value console, a **crash button** that really `SIGKILL`s the server (then
restarts it so recovery replays the WAL and the committed data returns), live
buffer-pool stats, and a security panel driving the real crypto/auth/audit
modules. Run it from the project root so it can find `web/dashboard.html`.

**The web bridge is an innovation layer only** — the assessed Task 4
client-server application is the AF_UNIX `durakv-server`/`durakv-client` pair
above, which contains no TCP/IP.

| Test | Proves |
|------|--------|
| `tests/test_ipc.c` | 8 concurrent clients × 50 ops over AF_UNIX, framed responses verified |
| `tests/demo_mqueue.c` | inter-process message sharing via a System V message queue |

## Security (Phase 5)

Built on **libsodium** (`brew install libsodium`); the security demos link it,
while the core engine stays dependency-free.

- **File permissions** (`tests/file_demo.c`) — the POSIX model directly:
  `open`/`creat` with a mode, `chmod` to change rwx, `access()` to query, and
  the kernel refusing a write to a `0444` file.
- **Encryption** (`src/crypto.c`) — XChaCha20-Poly1305 AEAD (chosen over
  AES-GCM: no hardware-AES needed, and a 192-bit nonce makes random nonces
  safe). The demo shows ciphertext (no plaintext visible) and that flipping one
  byte makes decryption **fail** — confidentiality *and* integrity.
- **Authentication** (`src/auth.c`) — passwords stored as **Argon2id** hashes
  (memory-hard); wrong passwords and unknown users are rejected identically.
- **Permissions** (`src/permissions.c`) — owner/group/other **rwx** on key
  namespaces, exactly like Unix file bits (first-match triad precedence).
- **Audit** (`src/audit.c`) — append-only, **hash-chained** log
  (`hash = SHA-256(prev || entry)`); editing any past entry breaks the chain,
  which `audit_verify()` detects and pinpoints.
- **Encryption at rest** (`src/encryption.c`) — opt-in via a master password
  (`DURAKV_PASSWORD`). A key is derived (Argon2id, salt in the header) and every
  page written to `data.db` *and* every page image logged to `wal.log` is
  sealed. `storage.c`/`recovery.c` stay libsodium-free, calling a codec through
  function pointers. Durability is unaffected: the crashtest still passes with
  encryption on.
- **Server enforcement** — with `DURAKV_SECURE=1` the server requires `AUTH`
  before data commands, checks rwx on each key's namespace, and writes every
  attempt (allowed or DENIED) to the audit log.

| Test | Proves |
|------|--------|
| `tests/file_demo.c` | POSIX create / chmod / access; write to a read-only file denied |
| `tests/demo_crypto.c` | AEAD confidentiality + integrity; Argon2id password auth |
| `tests/demo_auth.c` | Argon2 login + namespace rwx allow/deny across users |
| `tests/demo_audit.c` | hash-chained audit detects a one-byte tamper |
| `tests/demo_encrypt.c` | data + WAL encrypted at rest; wrong password locked out |
| `tests/test_secure.c` | server AUTH gate + rwx enforcement + intact audit chain |

Build the security demos (need libsodium): `make demo_crypto demo_audit demo_auth`.

## Layout

```
include/  storage.h wal.h recovery.h bufferpool.h replacement.h
          threadpool.h scheduler.h protocol.h server.h
          crypto.h auth.h permissions.h audit.h
src/      storage.c wal.c recovery.c bufferpool.c replacement.c
          threadpool.c scheduler.c protocol.c server.c client.c durakv.c
          crypto.c auth.c permissions.c audit.c encryption.c webserver.c
web/      dashboard.html                 (browser control room)
tests/    test_storage.c test_wal_recovery.c test_bufferpool.c test_belady.c
          mem_demo.c demo_race.c demo_deadlock.c demo_scheduler.c loadtest.c
          demo_mqueue.c test_ipc.c
          file_demo.c demo_crypto.c demo_auth.c demo_audit.c demo_encrypt.c
          test_secure.c
scripts/  crashtest.sh run_demo.sh
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
| `tests/test_ipc.c` / `demo_mqueue.c` | network/IPC (see the Phase 4 table above) |
| `tests/file_demo.c` / `demo_*` / `test_secure.c` | security (see the Phase 5 table above) |
| `scripts/crashtest.sh` | committed keys survive repeated `kill -9`; `make crashtest_concurrent` does it with 4 writer threads |
