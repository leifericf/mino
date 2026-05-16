/*
 * stencil_extract/selftest.h -- per-format synthetic-blob unit
 * tests. Each format's tests build a tiny in-memory object file
 * (Mach-O / ELF / COFF) with a known body, symbol table, and
 * relocation table, then runs the parser against it and asserts
 * the extracted records match expected values. The aggregate
 * `selftest_all` driver calls each in turn.
 *
 * Independent of compiling the project's actual .c files into .o,
 * this exercise catches parser regressions in isolation -- e.g.,
 * a struct layout drift on a new compiler, a missing reloc-kind
 * map entry, an off-by-one in a per-format extract loop.
 */

#ifndef STENCIL_EXTRACT_SELFTEST_H
#define STENCIL_EXTRACT_SELFTEST_H

/* Returns the number of failed asserts. Prints failures to stderr. */
int selftest_macho_synthetic(void);
int selftest_elf_synthetic(void);
int selftest_coff_synthetic(void);

#endif /* STENCIL_EXTRACT_SELFTEST_H */
