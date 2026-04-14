# mino — pure ANSI C build.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2
LDFLAGS ?=

SRCS    := mino.c main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := mino

.PHONY: all clean test test-gc-stress bench

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c mino.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) bench/vector_bench bench/vector_bench.o \
	      examples/embed examples/embed.o

test: $(TARGET)
	./tests/smoke.sh

# Collect on every allocation: exercises the marker and sweeper on each
# alloc site and catches any caller that holds unrooted pointers across
# allocation boundaries. Slower than `test` but the same suite.
test-gc-stress: $(TARGET)
	MINO_GC_STRESS=1 ./tests/smoke.sh

# Bench targets are built on demand; not wired into `all` or CI.
bench: bench/vector_bench
	./bench/vector_bench

bench/vector_bench: bench/vector_bench.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ bench/vector_bench.c mino.o

# Example embedding program.
example: examples/embed
	./examples/embed

examples/embed: examples/embed.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ examples/embed.c mino.o
