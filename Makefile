# DuraKV -- durable storage engine + in-RAM buffer pool. Only dependency so
# far is pthreads (in libc), used by the buffer pool.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude -pthread
LDFLAGS ?= -pthread

CORE    := src/storage.c src/wal.c src/recovery.c \
           src/bufferpool.c src/replacement.c

.PHONY: all tests test crashtest clean

all: durakv tests

durakv: $(CORE) src/durakv.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- unit tests ----------------------------------------------------------
tests: test_storage test_wal_recovery test_bufferpool mem_demo

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_wal_recovery: $(CORE) tests/test_wal_recovery.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_bufferpool: $(CORE) tests/test_bufferpool.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mem_demo: tests/mem_demo.c
	$(CC) $(CFLAGS) -o $@ $^

test: tests
	@echo "== test_storage =="      && ./test_storage
	@echo "== test_wal_recovery ==" && ./test_wal_recovery
	@echo "== test_bufferpool =="   && ./test_bufferpool
	@echo "== mem_demo =="          && ./mem_demo

crashtest: durakv
	./scripts/crashtest.sh

clean:
	rm -f durakv test_storage test_wal_recovery test_bufferpool test_belady mem_demo
	rm -f *.o
	rm -f *.db *.log /tmp/durakv_*.db /tmp/durakv_*.log
	rm -f /tmp/durakv_crash.out /tmp/durakv_crash.acked /tmp/durakv_recovery.db.snap
	rm -rf *.dSYM
