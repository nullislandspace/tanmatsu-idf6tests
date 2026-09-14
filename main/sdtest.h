#pragma once

#include <stdbool.h>
#include <stddef.h>

// Receives one line of human-readable report text (no trailing newline).
typedef void (*sdtest_log_fn)(const char* line);

typedef enum {
    SDTEST_OK    = 0,  // every case read correctly
    SDTEST_BAD   = 1,  // at least one case read wrong data
    SDTEST_ERROR = 2,  // could not run (no pattern file, unknown suite, ...)
} sdtest_status_t;

// Run one suite. `args` is a space-separated list of key=value options (see
// sdtest.c for the keys each suite understands). Emits CASE and SUMMARY records
// and human-readable lines through `log`.
sdtest_status_t sdtest_run(const char* suite, const char* args, sdtest_log_fn log);

// Look up a key=value option in an argument string.
long sdtest_arg_int(const char* args, const char* key, long def);
bool sdtest_arg_str(const char* args, const char* key, char* out, size_t outlen);
