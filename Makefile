# DuraKV -- durable storage engine, buffer pool, concurrency, AF_UNIX IPC and a
# security layer. The core engine needs only pthreads (in libc); libsodium is
# confined to the security demos below (crypto/auth) via SODIUM_CFLAGS/LIBS.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude -pthread
LDFLAGS ?= -pthread

CORE    := src/storage.c src/wal.c src/recovery.c \
           src/bufferpool.c src/replacement.c \
           src/threadpool.c src/scheduler.c
NET     := src/protocol.c
SEC     := src/crypto.c src/auth.c src/permissions.c src/audit.c
ENCSRC  := src/crypto.c src/encryption.c          # encryption-at-rest codec

# libsodium (security layer only) -- located via Homebrew, fallback /usr/local
SODIUM_PREFIX ?= $(shell brew --prefix libsodium 2>/dev/null || echo /usr/local)
SODIUM_CFLAGS := -I$(SODIUM_PREFIX)/include
SODIUM_LIBS   := -L$(SODIUM_PREFIX)/lib -lsodium

.PHONY: all tests test demo crashtest crashtest_concurrent clean

all: durakv durakv-server durakv-client durakv-web tests

durakv: $(CORE) $(ENCSRC) src/durakv.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

# --- network/IPC binaries (AF_UNIX) --------------------------------------
durakv-server: $(CORE) $(NET) $(SEC) src/encryption.c src/server.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

durakv-client: $(NET) src/client.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- web dashboard bridge (http on localhost -> af_unix server) -----------
# demo layer only: drives the real durakv-server from a browser. needs sodium
# because the security panel runs the real crypto/auth/audit modules.
durakv-web: $(NET) $(SEC) src/webserver.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

# --- unit tests ----------------------------------------------------------
tests: test_storage test_wal_recovery test_bufferpool test_belady mem_demo \
       demo_race demo_deadlock demo_scheduler loadtest test_ipc demo_mqueue test_secure \
       file_demo demo_crypto demo_auth demo_audit demo_encrypt

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_wal_recovery: $(CORE) tests/test_wal_recovery.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_bufferpool: $(CORE) tests/test_bufferpool.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_belady: $(CORE) tests/test_belady.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mem_demo: tests/mem_demo.c
	$(CC) $(CFLAGS) -o $@ $^

file_demo: tests/file_demo.c
	$(CC) $(CFLAGS) -o $@ $^

# --- security demos (need libsodium) -------------------------------------
demo_crypto: src/crypto.c tests/demo_crypto.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS)

demo_auth: src/crypto.c src/auth.c src/permissions.c tests/demo_auth.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS)

demo_audit: src/audit.c tests/demo_audit.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(SODIUM_LIBS)

demo_encrypt: $(CORE) $(ENCSRC) tests/demo_encrypt.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

# --- network/IPC tests ---------------------------------------------------
test_ipc: $(CORE) $(NET) $(SEC) src/encryption.c src/server.c tests/test_ipc.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -DDURAKV_SERVER_NO_MAIN -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

test_secure: $(CORE) $(NET) $(SEC) src/encryption.c src/server.c tests/test_secure.c
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) -DDURAKV_SERVER_NO_MAIN -o $@ $^ $(LDFLAGS) $(SODIUM_LIBS)

demo_mqueue: tests/demo_mqueue.c
	$(CC) $(CFLAGS) -o $@ $^

# --- concurrency demos ---------------------------------------------------
demo_race: $(CORE) tests/demo_race.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

demo_deadlock: $(CORE) tests/demo_deadlock.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

demo_scheduler: $(CORE) tests/demo_scheduler.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

loadtest: $(CORE) tests/loadtest.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: tests
	@echo "== test_storage =="      && ./test_storage
	@echo "== test_wal_recovery ==" && ./test_wal_recovery
	@echo "== test_bufferpool =="   && ./test_bufferpool
	@echo "== test_belady =="       && ./test_belady
	@echo "== mem_demo =="          && ./mem_demo
	@echo "== demo_race =="         && ./demo_race
	@echo "== demo_deadlock =="     && ./demo_deadlock
	@echo "== demo_scheduler =="    && ./demo_scheduler
	@echo "== loadtest =="          && ./loadtest
	@echo "== test_ipc =="          && ./test_ipc
	@echo "== test_secure =="       && ./test_secure
	@echo "== demo_mqueue =="       && ./demo_mqueue
	@echo "== file_demo =="         && ./file_demo
	@echo "== demo_crypto =="       && ./demo_crypto
	@echo "== demo_auth =="         && ./demo_auth
	@echo "== demo_audit =="        && ./demo_audit
	@echo "== demo_encrypt =="      && ./demo_encrypt

demo: tests
	./scripts/run_demo.sh

crashtest: durakv
	./scripts/crashtest.sh

crashtest_concurrent: durakv
	./scripts/crashtest.sh 12 400 4    # 4 writer threads per batch

clean:
	rm -f durakv durakv-server durakv-client durakv-web
	rm -f test_storage test_wal_recovery test_bufferpool test_belady mem_demo
	rm -f *.sock /tmp/durakv_*.sock
	rm -f demo_race demo_deadlock demo_scheduler loadtest test_ipc demo_mqueue file_demo test_secure demo_crypto demo_auth demo_audit demo_encrypt
	rm -f *.o
	rm -f *.audit
	rm -f *.db *.log /tmp/durakv_*.db /tmp/durakv_*.log
	rm -f /tmp/durakv_crash.out /tmp/durakv_crash.acked /tmp/durakv_recovery.db.snap
	rm -rf *.dSYM
