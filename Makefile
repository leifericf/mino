# mino -- pure ANSI C build.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wpedantic -Wextra -O2 -Isrc
LDFLAGS ?=
LIBS    ?= -lm

LIB_SRCS := src/mino.c src/diag.c src/eval_special.c \
            src/eval_special_defs.c src/eval_special_bindings.c \
            src/eval_special_control.c src/eval_special_fn.c \
            src/runtime_state.c src/runtime_var.c \
            src/runtime_error.c src/runtime_env.c src/runtime_gc.c \
            src/val.c src/vec.c src/map.c src/rbtree.c src/read.c src/print.c \
            src/prim.c src/prim_numeric.c src/prim_collections.c \
            src/prim_sequences.c src/prim_string.c src/prim_io.c \
            src/prim_reflection.c src/prim_meta.c src/prim_regex.c \
            src/prim_stateful.c src/prim_module.c \
            src/prim_fs.c \
            src/prim_host.c src/host_interop.c \
            src/clone.c src/re.c \
            src/async_buffer.c src/async_channel.c \
            src/async_handler.c src/async_select.c \
            src/async_scheduler.c src/async_timer.c src/prim_async.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
SRCS     := $(LIB_SRCS) main.c
OBJS     := $(SRCS:.c=.o)
TARGET   := mino

.PHONY: all clean test test-external qa-arch

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
src/prim.o: src/prim.c src/prim_internal.h src/mino_internal.h src/core_mino.h
src/prim_numeric.o: src/prim_numeric.c src/prim_internal.h src/mino_internal.h
src/prim_collections.o: src/prim_collections.c src/prim_internal.h src/mino_internal.h
src/prim_sequences.o: src/prim_sequences.c src/prim_internal.h src/mino_internal.h
src/prim_string.o: src/prim_string.c src/prim_internal.h src/mino_internal.h
src/prim_io.o: src/prim_io.c src/prim_internal.h src/mino_internal.h
src/prim_reflection.o: src/prim_reflection.c src/prim_internal.h src/mino_internal.h
src/prim_meta.o: src/prim_meta.c src/prim_internal.h src/mino_internal.h
src/prim_regex.o: src/prim_regex.c src/prim_internal.h src/mino_internal.h src/re.h
src/prim_stateful.o: src/prim_stateful.c src/prim_internal.h src/mino_internal.h
src/prim_module.o: src/prim_module.c src/prim_internal.h src/mino_internal.h
src/prim_fs.o: src/prim_fs.c src/prim_internal.h src/mino_internal.h
src/prim_host.o: src/prim_host.c src/prim_internal.h src/mino_internal.h
src/host_interop.o: src/host_interop.c src/mino_internal.h
src/async_buffer.o: src/async_buffer.c src/async_buffer.h src/mino_internal.h
src/async_channel.o: src/async_channel.c src/async_channel.h src/async_buffer.h src/async_scheduler.h src/mino_internal.h
src/async_handler.o: src/async_handler.c src/async_handler.h src/mino_internal.h
src/async_select.o: src/async_select.c src/async_select.h src/async_channel.h src/async_scheduler.h src/prim_internal.h
src/async_scheduler.o: src/async_scheduler.c src/async_scheduler.h src/async_timer.h src/mino_internal.h
src/async_timer.o: src/async_timer.c src/async_timer.h src/async_channel.h src/mino_internal.h
src/prim_async.o: src/prim_async.c src/prim_internal.h src/async_buffer.h src/async_channel.h src/async_scheduler.h src/async_select.h src/mino_internal.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) src/core_mino.h

test: $(TARGET)
	./mino tests/run.mino

test-external: $(TARGET)
	./mino tests/external_runner.mino

# Architecture quality gates (placeholder — rewrite in mino).
qa-arch:
	@echo "qa-arch: not yet reimplemented in mino"
