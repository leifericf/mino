# mino — pure ANSI C build.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2
LDFLAGS ?=

SRCS    := mino.c main.c
OBJS    := $(SRCS:.c=.o)
TARGET  := mino

.PHONY: all clean test test-gc-stress bench bench-map bench-seq fuzz-stdin fuzz-crash

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

# stdlib.mino is compiled into a C header so it can be #included.
stdlib_mino.h: stdlib.mino
	@printf 'static const char *stdlib_mino_src =\n' > $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$$/\\n"/' $< >> $@
	@printf '    ;\n' >> $@

mino.o: mino.c mino.h stdlib_mino.h

%.o: %.c mino.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) stdlib_mino.h bench/vector_bench bench/vector_bench.o \
	      bench/map_bench bench/map_bench.o bench/seq_bench bench/seq_bench.o \
	      fuzz/fuzz_reader fuzz/fuzz_reader.o \
	      examples/embed examples/embed.o \
	      cookbook/config cookbook/rules cookbook/repl_socket \
	      cookbook/plugin cookbook/pipeline cookbook/console

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

bench-map: bench/map_bench
	./bench/map_bench

bench/map_bench: bench/map_bench.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ bench/map_bench.c mino.o

bench-seq: bench/seq_bench
	./bench/seq_bench

bench/seq_bench: bench/seq_bench.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ bench/seq_bench.c mino.o

# Example embedding program.
example: examples/embed
	./examples/embed

examples/embed: examples/embed.c mino.o mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ examples/embed.c mino.o

# Fuzz targets: stdin mode for crash_test.sh; libFuzzer for CI.
fuzz-stdin: fuzz/fuzz_reader
	@echo "fuzz_reader built (stdin mode). Run: ./fuzz/crash_test.sh"

fuzz/fuzz_reader: fuzz/fuzz_reader.c mino.c mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -DFUZZ_STDIN -I. -o $@ fuzz/fuzz_reader.c mino.c

fuzz-crash: fuzz/fuzz_reader
	./fuzz/crash_test.sh
