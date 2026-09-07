# Bootstrap Makefile -- the smallest recipe that turns a clean checkout
# into a working `./mino` binary. After this, every other build, test,
# release, and tooling task lives in mino code:
#
#   make            # gen bundled-source headers and compile ./mino
#   make clean      # remove ./mino and the generated headers
#   ./mino task     # list available tasks
#   ./mino task build / test / build-asan / ...
#
# Anything beyond bootstrap belongs in lib/mino/tasks/builtin.clj. Do
# not grow this Makefile -- if you find yourself adding a target here,
# add it to the task runner instead.
#
# Compile-out flags. The default build includes every capability (no
# flag needed). Each MINO_NO_<CAP> subtracts one large optional
# capability so "what mino is" (the always-on floor: reader, printer,
# eval, collections, core prims) is separable from "what a given build
# contains". The floor can never be compiled out. Pass them via CFLAGS,
# e.g. `make CFLAGS="... -DMINO_NO_TLS"`, or use `./mino task build-min`,
# which sets all of them at once:
#
#   MINO_NO_TLS       drop the vendored BearSSL TLS client and the
#                     bundled CA roots (~71k LOC). https / tls-connect
#                     throw :tls/unavailable; plain http, digest, and
#                     websocket keep working (hashes survive in
#                     src/vendor/bearssl/bearssl_hash.c).
#   MINO_NO_TZDATA    drop the IANA timezone database blob (~10.7k LOC).
#                     UTC and fixed offsets still work; named-zone lookup
#                     throws a classified error.
#   MINO_NO_DOCUMENTS drop the document parsers (html + html-entities +
#                     yaml + toml, ~8.6k LOC). Their parse / emit prims
#                     throw a classified error.

CC      ?= cc
# -Wno-clobbered silences a gcc-specific false positive in the bytecode VM
# (mino_bc_run holds locals across setjmp/longjmp pairs; the warning is a
# heuristic that doesn't reflect whether the value is actually re-read on
# the longjmp branch). Apple clang doesn't emit this warning at all and
# doesn't recognise the flag name, so the -Wno-unknown-warning-option
# pair lets us pass -Wno-clobbered to both compilers without breaking
# either.
# -fno-strict-aliasing: the vendored BearSSL and miniz type-pun byte
# buffers through wider integer pointers on their unaligned-access fast
# paths (BR_LE_UNALIGNED). gcc -O2 exploits the aliasing rule and
# miscompiles them on x86_64 (observed: wrong MD5 output); the flag keeps
# that third-party code correct at -O2.
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -Werror -Wno-missing-field-initializers -Wno-unknown-warning-option -Wno-clobbered -O2 -fno-strict-aliasing -DMINO_CPJIT=1
INCDIRS  = -Isrc -Isrc/generated -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval \
           -Isrc/read -Isrc/print -Isrc/names -Isrc/state \
           -Isrc/values -Isrc/collections -Isrc/prim -Isrc/async \
           -Isrc/interop -Isrc/diag -Isrc/vendor/imath \
           -Isrc/vendor/bearssl -Isrc/vendor/bearssl/inc \
           -Isrc/vendor/miniz -Isrc/vendor/miniz/upstream

ifeq ($(OS),Windows_NT)
EXE  = .exe
# bcrypt: the TLS layer's BCryptGenRandom entropy seeder links
# against it (src/prim/tls.c replaces BearSSL's vendored sysrng unit).
LIBS = -lm -lws2_32 -lbcrypt
# Static link on Windows so mino.exe doesn't depend on mingw runtime
# DLLs (libgcc_s_seh-1.dll, libwinpthread-1.dll). Without -static the
# exe fails to start on a fresh Windows install with
# STATUS_DLL_NOT_FOUND (0xC0000135) — the GHA runner has the DLLs,
# but a Scoop / Homebrew end user doesn't.
LDFLAGS += -static
# mingw's ld defaults the PE main-thread stack reserve to 2 MiB;
# POSIX hosts give the main thread 8 MiB (and mino spawns every
# worker with 8 MiB). Deep-but-legal recursion in the regex matcher
# (matchgroup cap 10000) and interpreter error paths overflow a
# 2 MiB main stack with STATUS_STACK_OVERFLOW (0xC00000FD) — seen
# as suite crashes on the windows-2022 runner. Match the 8 MiB
# default every other host assumes (MINO_WORKER_STACK_DEFAULT).
LDFLAGS += -Wl,--stack,8388608
else
EXE  =
LIBS = -lm -lpthread
endif

BIN = mino$(EXE)

SRCS = $(wildcard src/eval/*.c src/eval/bc/*.c src/eval/bc/jit/*.c \
                  src/read/*.c src/print/*.c \
                  src/diag/*.c \
                  src/names/*.c src/state/*.c src/gc/*.c src/public/*.c \
                  src/values/*.c src/collections/*.c src/prim/*/*.c \
                  src/interop/*.c src/regex/*.c src/async/*.c \
                   src/vendor/imath/*.c \
                   src/vendor/bearssl/*.c \
                   src/vendor/miniz/*.c) src/cli/*.c

# Bundled-source header set: <c-symbol>:<source-path> pairs. Each entry
# becomes src/generated/<symbol>.h with a single static const char
# *<symbol>_src C string literal. src/bundled.list is the single source of truth for
# which namespaces are bundled; builtin.clj's `bundled-stdlib` parses
# the same file for incremental `./mino task build` rebuilds. This
# bootstrap path reads it here so there is exactly one list to edit.
#
# For each source path the c-symbol is derived by dropping the .clj
# suffix and turning every non-alphanumeric character into _
# (lib/clojure/core/async.clj -> lib_clojure_core_async), matching the
# namespace-to-symbol rule both bundlers already share. core.clj is
# bundled separately as core_mino (it is the runtime core, not a lib).
BUNDLED_LIBS = $(shell awk '$$1 ~ /\.clj$$/ { sym = $$1; sub(/\.clj$$/, "", sym); gsub(/[^A-Za-z0-9]/, "_", sym); print sym ":" $$1 }' src/bundled.list)
BUNDLED = core_mino:src/core.clj $(BUNDLED_LIBS)

HEADERS = $(foreach p,$(BUNDLED),src/generated/$(word 1,$(subst :, ,$(p))).h)

.PHONY: bootstrap clean
bootstrap: $(BIN)

$(BIN): $(HEADERS)
	$(CC) $(CFLAGS) $(INCDIRS) -o $@ $(SRCS) $(LDFLAGS) $(LIBS)

# One recipe regenerates the entire bundled-source header set.
# Triggered when any header is missing or older than this Makefile;
# day-to-day incremental rebuilds use `./mino task build` instead.
#
# The escape script lives in src/bundle.awk rather than inline. Both
# Git Bash sed and Git Bash awk's command-line argument handling on
# Windows put MSYS path translation between the shell and the tool:
# inline regex literals like `/\\/` look path-shaped and get
# rewritten before the tool parses them. Reading the script from a
# file keeps it off the command line entirely; -f's argument is the
# file path, which path translation handles correctly. bundle.awk
# writes the whole header, banner included, so its bytes match what
# `gen-core-header` / `gen-stdlib-headers` emit for every entry.
$(HEADERS): src/bundle.awk src/bundled.list Makefile
	@mkdir -p src/generated
	@for pair in $(BUNDLED); do \
	    sym=$${pair%%:*}; \
	    src=$${pair##*:}; \
	    awk -v sym="$$sym" -v src="$$src" -f src/bundle.awk "$$src" > "src/generated/$$sym.h"; \
	done

clean:
	rm -f $(BIN) $(HEADERS)
