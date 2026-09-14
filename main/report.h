// Machine-readable result records on the debug console.
//
// Each record is one line: `@@SDT-<KIND>@@ <json> @@<crc32>@@`. The CRC covers
// the JSON and matches Python's zlib.crc32, so tools/sdtest.py can drop lines
// that another task's log output landed in the middle of.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define REPORT_JSON_MAX 1536

uint32_t report_crc32(void const* data, size_t len);

void report_emit(char const* kind, char const* json);
void report_emitf(char const* kind, char const* fmt, ...) __attribute__((format(printf, 2, 3)));
