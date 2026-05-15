/*
 * stencils/return.c -- copy-and-patch stencil source for the simplest
 * OP_RETURN shape: hand back the value already sitting in argument 0.
 *
 * The body is intentionally minimal so the build pipeline (mino's
 * stencil_extract -> generated header -> runtime memcpy) can be
 * validated end-to-end before the immediate-patching infrastructure
 * lands. A general-purpose OP_RETURN with arbitrary A-register
 * encoding joins when the JIT runtime is ready to consume relocations
 * (the same release that wires the mmap + mprotect path).
 *
 * Build: this file is compiled as part of the stencil pipeline only;
 * the runtime never compiles or links it directly. The compiled .o is
 * fed to tools/stencil_extract which writes the byte table into
 * src/eval/bc/stencils/generated/stencils_<arch>_<os>.h.
 */

#include <stddef.h>

void *stencil_op_return_arg0(void **arg0)
{
    return arg0[0];
}
