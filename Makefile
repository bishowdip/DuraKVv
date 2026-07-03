# DuraKV -- Phase 1 (storage engine). No external dependencies yet.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude

CORE    := src/storage.c src/wal.c

.PHONY: all tests test clean

all: tests

# --- unit tests ----------------------------------------------------------
tests: test_storage

test_storage: $(CORE) tests/test_storage.c
	$(CC) $(CFLAGS) -o $@ $^

test: tests
	@echo "== test_storage ==" && ./test_storage

clean:
	rm -f test_storage
	rm -f *.o
	rm -f *.db *.log /tmp/durakv_*.db
	rm -rf *.dSYM
