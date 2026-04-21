# Thin wrapper: delegates to `mino task` for everything.
# Only bootstraps the binary if it doesn't exist yet.

CC     ?= cc
CFLAGS ?= -std=c99 -Wall -Wpedantic -Wextra -O2 -Isrc
LIBS   ?= -lm
OBJS   := $(patsubst %.c,%.o,$(wildcard src/*.c) main.c)

.PHONY: all clean test test-external bootstrap

all: bootstrap
	./mino task build

test: bootstrap
	./mino task test

test-external: bootstrap
	./mino task test-external

clean:
	@if test -x ./mino; then ./mino task clean; \
	else rm -f $(OBJS) mino src/core_mino.h; fi

# Bootstrap: cold-compile mino so `mino task` can run.
bootstrap:
	@test -x ./mino || $(MAKE) --no-print-directory _bootstrap

_bootstrap: src/core_mino.h $(OBJS)
	$(CC) $(CFLAGS) -o mino $(OBJS) $(LIBS)

src/core_mino.h: src/core.mino
	@printf 'static const char *core_mino_src =\n' > $@
	@sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$$/\\n"/' $< >> $@
	@printf '    ;\n' >> $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
