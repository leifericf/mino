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

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2
INCDIRS  = -Isrc -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval \
           -Isrc/collections -Isrc/prim -Isrc/async -Isrc/interop \
           -Isrc/diag -Isrc/vendor/imath

ifeq ($(OS),Windows_NT)
EXE  = .exe
LIBS = -lm
# Static link on Windows so mino.exe doesn't depend on mingw runtime
# DLLs (libgcc_s_seh-1.dll, libwinpthread-1.dll). Without -static the
# exe fails to start on a fresh Windows install with
# STATUS_DLL_NOT_FOUND (0xC0000135) — the GHA runner has the DLLs,
# but a Scoop / Homebrew end user doesn't.
LDFLAGS += -static
else
EXE  =
LIBS = -lm -lpthread
endif

BIN = mino$(EXE)

SRCS = $(wildcard src/eval/*.c src/diag/*.c src/runtime/*.c \
                  src/gc/*.c src/public/*.c src/collections/*.c \
                  src/prim/*.c src/interop/*.c src/regex/*.c \
                  src/async/*.c src/vendor/imath/*.c) main.c

# Bundled-source header set: <c-symbol>:<source-path> pairs. Each entry
# becomes src/<symbol>.h with a single static const char *<symbol>_src
# C string literal. Keep this list in sync with `bundled-stdlib` in
# lib/mino/tasks/builtin.clj; that one drives the incremental rebuilds
# under `./mino task build`, this one drives the from-scratch bootstrap.
BUNDLED = \
    core_mino:src/core.clj \
    lib_clojure_string:lib/clojure/string.clj \
    lib_clojure_set:lib/clojure/set.clj \
    lib_clojure_walk:lib/clojure/walk.clj \
    lib_clojure_edn:lib/clojure/edn.clj \
    lib_clojure_pprint:lib/clojure/pprint.clj \
    lib_clojure_zip:lib/clojure/zip.clj \
    lib_clojure_data:lib/clojure/data.clj \
    lib_clojure_test:lib/clojure/test.clj \
    lib_clojure_template:lib/clojure/template.clj \
    lib_clojure_repl:lib/clojure/repl.clj \
    lib_clojure_stacktrace:lib/clojure/stacktrace.clj \
    lib_clojure_datafy:lib/clojure/datafy.clj \
    lib_clojure_core_protocols:lib/clojure/core/protocols.clj \
    lib_clojure_instant:lib/clojure/instant.clj \
    lib_clojure_spec_alpha:lib/clojure/spec/alpha.clj \
    lib_clojure_core_specs_alpha:lib/clojure/core/specs/alpha.clj \
    lib_mino_deps:lib/mino/deps.clj \
    lib_mino_tasks:lib/mino/tasks.clj \
    lib_mino_tasks_builtin:lib/mino/tasks/builtin.clj

HEADERS = $(foreach p,$(BUNDLED),src/$(word 1,$(subst :, ,$(p))).h)

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
# file path, which path translation handles correctly.
$(HEADERS): src/bundle.awk Makefile
	@for pair in $(BUNDLED); do \
	    sym=$${pair%%:*}; \
	    src=$${pair##*:}; \
	    out=src/$$sym.h; \
	    printf 'static const char *%s_src =\n' "$$sym" > "$$out"; \
	    awk -f src/bundle.awk "$$src" >> "$$out"; \
	    printf '    ;\n' >> "$$out"; \
	done

clean:
	rm -f $(BIN) $(HEADERS)
