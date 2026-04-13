# mino — pure ANSI C build.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2
LDFLAGS ?=

SRCS    := mino.c main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := mino

.PHONY: all clean test bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c mino.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) bench/vector_bench bench/vector_bench.o

test: $(TARGET)
	./tests/smoke.sh

# Bench targets are built on demand; not wired into `all` or CI.
bench: bench/vector_bench
	./bench/vector_bench

bench/vector_bench: bench/vector_bench.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ bench/vector_bench.c mino.o
