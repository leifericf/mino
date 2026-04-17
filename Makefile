# mino -- pure ANSI C build.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2 -Isrc
LDFLAGS ?=
LIBS    ?= -lm

LIB_SRCS := src/mino.c src/eval_special.c \
            src/eval_special_defs.c src/eval_special_bindings.c \
            src/eval_special_control.c src/eval_special_fn.c \
            src/runtime_state.c \
            src/runtime_error.c src/runtime_env.c src/runtime_gc.c \
            src/val.c src/vec.c src/map.c src/read.c src/print.c \
            src/prim.c src/prim_numeric.c src/prim_collections.c \
            src/prim_sequences.c src/prim_string.c src/prim_io.c \
            src/clone.c src/re.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
SRCS     := $(LIB_SRCS) main.c
OBJS     := $(SRCS:.c=.o)
TARGET   := mino

.PHONY: all clean test test-gc-stress test-gc-stress-sharded \
       test-fault-inject test-regex-thread \
       bench bench-map bench-seq fuzz-stdin qa-arch

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# core.mino is compiled into a C header so it can be #included.
src/core_mino.h: src/core.mino
	@printf 'static const char *core_mino_src =\n' > $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$$/\\n"/' $< >> $@
	@printf '    ;\n' >> $@

src/mino.o: src/mino.c src/mino.h src/mino_internal.h
src/eval_special.o: src/eval_special.c src/eval_special_internal.h src/mino_internal.h
src/eval_special_defs.o: src/eval_special_defs.c src/eval_special_internal.h src/mino_internal.h
src/eval_special_bindings.o: src/eval_special_bindings.c src/eval_special_internal.h src/mino_internal.h
src/eval_special_control.o: src/eval_special_control.c src/eval_special_internal.h src/mino_internal.h
src/eval_special_fn.o: src/eval_special_fn.c src/eval_special_internal.h src/mino_internal.h
src/prim.o: src/prim.c src/prim_internal.h src/mino_internal.h src/core_mino.h src/re.h
src/prim_numeric.o: src/prim_numeric.c src/prim_internal.h src/mino_internal.h
src/prim_collections.o: src/prim_collections.c src/prim_internal.h src/mino_internal.h
src/prim_sequences.o: src/prim_sequences.c src/prim_internal.h src/mino_internal.h
src/prim_string.o: src/prim_string.c src/prim_internal.h src/mino_internal.h
src/prim_io.o: src/prim_io.c src/prim_internal.h src/mino_internal.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) src/core_mino.h bench/vector_bench bench/vector_bench.o \
	      bench/map_bench bench/map_bench.o bench/seq_bench bench/seq_bench.o \
	      fuzz/fuzz_reader fuzz/fuzz_reader.o \
	      examples/embed examples/embed.o \
	      examples/fault_inject_test examples/fault_inject_test.o \
	      examples/regex_thread_test examples/regex_thread_test.o \
	      cookbook/config cookbook/rules cookbook/repl_socket \
	      cookbook/plugin cookbook/pipeline cookbook/console

test: $(TARGET)
	./mino tests/run.mino

# Collect on every allocation: exercises the marker and sweeper on each
# alloc site and catches any caller that holds unrooted pointers across
# allocation boundaries. Slower than `test` but the same suite.
test-gc-stress: $(TARGET)
	MINO_GC_STRESS=1 ./mino tests/run.mino

# Sharded GC stress: same coverage, split into shards to avoid
# watchdog timeouts.  Expected total: ~5 minutes on a modern machine.
# Run all shards sequentially:
#   make test-gc-stress-sharded
# Or run a single shard:
#   MINO_GC_STRESS=1 ./mino tests/run_gc_shard3.mino
test-gc-stress-sharded: $(TARGET)
	@for i in 1 2 3 4 5 6 7 8 9 10 11; do \
	  printf "shard %s/11... " "$$i"; \
	  MINO_GC_STRESS=1 ./mino tests/run_gc_shard$$i.mino || exit 1; \
	done
	@echo "all shards passed"

# Bench targets are built on demand; not wired into `all` or CI.
bench: bench/vector_bench
	./bench/vector_bench

bench/vector_bench: bench/vector_bench.c $(LIB_OBJS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bench/vector_bench.c $(LIB_OBJS) $(LIBS)

bench-map: bench/map_bench
	./bench/map_bench

bench/map_bench: bench/map_bench.c $(LIB_OBJS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bench/map_bench.c $(LIB_OBJS) $(LIBS)

bench-seq: bench/seq_bench
	./bench/seq_bench

bench/seq_bench: bench/seq_bench.c $(LIB_OBJS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ bench/seq_bench.c $(LIB_OBJS) $(LIBS)

# Example embedding program.
example: examples/embed
	./examples/embed

examples/embed: examples/embed.c $(LIB_OBJS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ examples/embed.c $(LIB_OBJS) $(LIBS)

# Fuzz targets: stdin mode for crash_test.sh; libFuzzer for CI.
# Fault-injection test: deterministic OOM recovery tests.
test-fault-inject: examples/fault_inject_test
	./examples/fault_inject_test

examples/fault_inject_test: examples/fault_inject_test.c $(LIB_OBJS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ examples/fault_inject_test.c $(LIB_OBJS) $(LIBS)

# Regex thread-safety smoke test (requires pthreads).
test-regex-thread: examples/regex_thread_test
	./examples/regex_thread_test

examples/regex_thread_test: examples/regex_thread_test.c src/re.c src/re.h
	$(CC) $(CFLAGS) -Isrc $(LDFLAGS) -pthread -o $@ examples/regex_thread_test.c src/re.c

fuzz-stdin: fuzz/fuzz_reader
	@echo "fuzz_reader built (stdin mode). Run: ./fuzz/crash_test.sh"

fuzz/fuzz_reader: fuzz/fuzz_reader.c $(LIB_SRCS) src/mino.h
	$(CC) $(CFLAGS) $(LDFLAGS) -DFUZZ_STDIN -o $@ fuzz/fuzz_reader.c $(LIB_SRCS) $(LIBS)

# Architecture quality gates: TU size, function span, abort inventory.
qa-arch:
	@sh scripts/qa-arch.sh
