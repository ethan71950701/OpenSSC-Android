/* Stage 2: log-only inline hooks. Every hook calls the original function with the original
 * arguments and returns its result unchanged; it only records what it saw. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "symres.h"

/* Installs all hooks or none. `bias` is the load bias of libbluetooth_jni.so. Returns 0 on success.
 * Targets must already be resolved (sr_scan); a required target that is missing or ambiguous fails. */
int hooks_install(uintptr_t bias, const sr_target *targets, size_t count, const char *log_path,
                  const char *stream_state_path);

/* Formats `n` bytes as lower-case hex into `out` (NUL-terminated, truncated to out_cap). */
void hooks_hex(const uint8_t *p, size_t n, char *out, size_t out_cap);
