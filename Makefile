# mino — example Makefile.
#
# The canonical build is still `./mino task build` (see mino.edn), which
# does incremental .o -> binary linking via the bundled task runner. This
# Makefile exists as a reference for embedders and as the switchboard for
# sanitizer development builds. It always rebuilds from scratch; if that
# hurts, use the task runner.
#
# Common targets:
#
#   make             — build the mino binary with -O2.
#   make examples    — build the in-tree embedder examples.
#   make dev-asan    — build ./mino_asan with AddressSanitizer.
#   make dev-ubsan   — build ./mino_ubsan with UndefinedBehaviorSanitizer.
#   make dev-tsan    — build ./mino_tsan with ThreadSanitizer.
#   make clean       — remove build artifacts (does not touch src/core_mino.h).
#
# Environment overrides: CC, CFLAGS, LDFLAGS, LIBS, SANFLAGS.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
LIBS    ?= -lm

SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.c) main.c
EXAMPLES_DIR := examples

MINO_BIN := mino

# AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer configs.
# -O1 + frame-pointer keep stack traces readable under sanitizer overhead.
SAN_BASE := -g -O1 -fno-omit-frame-pointer
ASAN_FLAGS  := $(SAN_BASE) -fsanitize=address
UBSAN_FLAGS := $(SAN_BASE) -fsanitize=undefined -fno-sanitize-recover=undefined
TSAN_FLAGS  := $(SAN_BASE) -fsanitize=thread

.PHONY: all examples clean dev-asan dev-ubsan dev-tsan

all: $(MINO_BIN)

$(MINO_BIN): $(SRCS)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $(LDFLAGS) -o $@ $^ $(LIBS)

dev-asan: mino_asan

mino_asan: $(SRCS)
	$(CC) $(ASAN_FLAGS) -std=c99 -Wall -Wextra -Wpedantic -I$(SRC_DIR) \
		$(LDFLAGS) -o $@ $^ $(LIBS)

dev-ubsan: mino_ubsan

mino_ubsan: $(SRCS)
	$(CC) $(UBSAN_FLAGS) -std=c99 -Wall -Wextra -Wpedantic -I$(SRC_DIR) \
		$(LDFLAGS) -o $@ $^ $(LIBS)

dev-tsan: mino_tsan

mino_tsan: $(SRCS)
	$(CC) $(TSAN_FLAGS) -std=c99 -Wall -Wextra -Wpedantic -I$(SRC_DIR) \
		$(LDFLAGS) -o $@ $^ $(LIBS)

# Each example is one .c file linked against every mino .c file. Embedders
# building their own apps should follow the same shape: add their sources
# to the $(SRC_DIR)/*.c list (or link against a libmino.a of their own).
EXAMPLE_SRCS := $(wildcard $(EXAMPLES_DIR)/*.c)
EXAMPLE_BINS := $(EXAMPLE_SRCS:.c=)
MINO_SRCS_ONLY := $(wildcard $(SRC_DIR)/*.c)

examples: $(EXAMPLE_BINS)

$(EXAMPLES_DIR)/%: $(EXAMPLES_DIR)/%.c $(MINO_SRCS_ONLY)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $(LDFLAGS) -o $@ $< $(MINO_SRCS_ONLY) $(LIBS)

clean:
	rm -f $(MINO_BIN) mino_asan mino_ubsan mino_tsan
	rm -f $(EXAMPLE_BINS)
	rm -rf *.dSYM $(EXAMPLES_DIR)/*.dSYM
