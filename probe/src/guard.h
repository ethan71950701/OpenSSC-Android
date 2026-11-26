/* Crash-loop guard: counts runs that did not survive long enough to report success. */
#pragma once

#define GUARD_LIMIT 3

/* Records the start of a run. Returns the new count (1..GUARD_LIMIT) if the run may proceed,
 * 0 if the limit was reached, -1 if the state cannot be read or written (do not proceed). */
int guard_begin(const char *path);
/* Resets the count after a run survived. Returns 0 on success. */
int guard_success(const char *path);

/* Current count without changing it: 0 if the file is missing, -1 if unreadable. */
int guard_count(const char *path);
