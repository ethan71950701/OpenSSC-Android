/* Finds functions by source name in an ELF64 file, including the symbols that only exist in
 * .gnu_debugdata (MiniDebugInfo). Read-only: never touches the loaded library. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SR_MAX_MATCH 4
#define SR_NAME_MAX 256

struct sr_target {
    const char *name; /* "func" or "Class::method" */
    int required;     /* needed by the first hook version */
    int matches;      /* number of distinct addresses found */
    struct {
        uint64_t value, size;
        char symbol[SR_NAME_MAX];
    } match[SR_MAX_MATCH];
};

enum sr_status {
    SR_OK = 0,
    SR_BAD_ELF = -1,
    SR_NO_SYMBOLS = -2,
    SR_BAD_XZ = -3,
    SR_NO_MEMORY = -4
};

/* 1 if the symbol (plain C or Itanium-mangled C++) is the function `name`, else 0. */
int sr_symbol_matches(const char *symbol, const char *name);

/* Scans .symtab and .gnu_debugdata of the ELF image in memory. Fills targets[].matches. */
int sr_scan(const uint8_t *file, size_t len, struct sr_target *targets, size_t count);
