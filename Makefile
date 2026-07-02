# DuraKV -- Phase 1 (storage engine). No external dependencies yet.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -Iinclude

.PHONY: all clean

all:

clean:
	rm -f *.o
	rm -f *.db *.log
	rm -rf *.dSYM
