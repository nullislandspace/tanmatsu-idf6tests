#pragma once

#include <stdbool.h>

// Receives one line of report text (no trailing newline).
typedef void (*sdtest_log_fn)(const char* line);

// Run the whole test: make sure the pattern file is on the card and intact,
// then read it back in every combination of destination, size and offset,
// and report each case. Returns true if every case read correctly.
bool sdtest_run(sdtest_log_fn log);
