/* Stage 3.3 diagnostics: log-only hooks on functions chosen at run time (names from diag_targets.txt in the
 * module directory). Each hook forwards the first six integer/pointer arguments and returns the first return
 * register unchanged, and records them. ONLY suitable for functions that take at most six integer or pointer
 * arguments, no floating point arguments, no stack arguments, and do not return a struct through a hidden
 * pointer (x8) or a floating point value. Pointers are logged as values, never dereferenced. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "symres.h"

#define DIAG_MAX 10
#define DIAG_NAME_MAX 160

/* Parses a diag_targets.txt image: one name per line, '#' starts a comment. Returns the number of names
 * stored in out[] (at most DIAG_MAX). */
size_t diag_parse_names(const char *text, size_t len, char out[][DIAG_NAME_MAX]);

/* Hooks every name that resolved to exactly one function in `targets`. A failure of one diagnostic hook is
 * logged and skipped; it never affects the main hooks. Returns the number of hooks installed. */
int diag_install(uintptr_t bias, const sr_target *targets, size_t count, char names[][DIAG_NAME_MAX], size_t n);
