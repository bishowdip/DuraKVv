# DuraKV -- Phase 1 (storage engine). No external dependencies yet.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude

CORE    := src/storage.c src/wal.c src/recovery.c

.PHONY: all tests test clean

all: durakv tests

durakv: $(CORE) src/durakv.c
	$(CC) $(CFLAGS) -o $@ $^

# --- unit tests ----------------------------------------------------------
tests: test_storage test_wal_recovery

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^

test_wal_recovery: $(CORE) tests/test_wal_recovery.c
	$(CC) $(CFLAGS) -o $@ $^

test: tests
	@echo "== test_storage =="      && ./test_storage
	@echo "== test_wal_recovery ==" && ./test_wal_recovery

clean:
	rm -f durakv test_storage test_wal_recovery
	rm -f *.o
	rm -f *.db *.log /tmp/durakv_*.db /tmp/durakv_*.log
	rm -rf *.dSYM
