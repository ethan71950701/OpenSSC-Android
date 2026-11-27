/* Stage 2: log-only inline hooks. Every hook calls the original function with the original
 * arguments and returns its result unchanged; it only records what it saw. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "symres.h"

/* Call before hooks_install. On: the receive-side hook rewrites SSC capabilities to Opus, the send-side hook
 * rewrites the Opus configuration back to SSC, and send_frames is muted while an SSC configuration is active
 * (no audio packet is sent). Off (default): hooks only log. */
void hooks_set_translate(int on);

/* Translate mode only: 1 (default) sends real SSC audio while an SSC configuration is active, 0 keeps muting. */
void hooks_set_ssc_audio(int on);

/* Media tick reported to the stack while SSC audio streams (default 18 ms = one frame); 0 keeps the stack's value. */
void hooks_set_ssc_interval(int ms);
/* SSC configuration byte sent to the Buds (default 0x0C HiFi 48 kHz; 0x0A = UHQ 96 kHz). */
void hooks_set_ssc_config(int byte);

/* Call before hooks_install (translate mode). Remembers across restarts that an SSC peer exists: a sighting
 * younger than 24 h arms AAC hiding at once, which a fresh connection needs because the AAC capability list is
 * fetched before the SSC one. */
void hooks_set_seen_path(const char *path);

/* Installs all hooks or none. `bias` is the load bias of libbluetooth_jni.so. Returns 0 on success.
 * Targets must already be resolved (sr_scan); a required target that is missing or ambiguous fails. */
int hooks_install(uintptr_t bias, const sr_target *targets, size_t count, const char *log_path,
                  const char *stream_state_path);

/* Formats `n` bytes as lower-case hex into `out` (NUL-terminated, truncated to out_cap). */
void hooks_hex(const uint8_t *p, size_t n, char *out, size_t out_cap);

/* Line logger shared with diag.cpp: logcat plus the hook log file, capped at a fixed number of lines. */
__attribute__((format(printf, 1, 2))) void hooks_log(const char *fmt, ...);

/* Stream crash guard upkeep. A stream armed by encoder_init is considered survived once the process has stayed
 * up for 60 s since then (same rule as the process guard); the probe thread calls hooks_stream_tick() every
 * few seconds. hooks_stream_tick_at is the testable form (now = CLOCK_MONOTONIC seconds + 1). */
void hooks_stream_tick(void);
void hooks_stream_tick_at(long now);
