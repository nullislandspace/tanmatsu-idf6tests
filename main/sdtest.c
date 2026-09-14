// SD card read test for the ESP-IDF 6 corruption described in
// idf6_sd_fuckup.md.
//
// The pattern file makes corruption self-describing: every 4-byte word at file
// offset `o` holds `o ^ PATTERN_KEY`. A byte that lands in the wrong place
// therefore says where it was meant to go, so the report can tell whole blocks
// that were shifted or duplicated from zeros, untouched memory or garbage.
//
// The file is written and baseline-verified through a 4 KB internal-RAM
// buffer, the path known to work (graceloader's workaround). The test reads
// then use single, unbuffered fread() calls -- so FatFs hands the whole request
// to the SDMMC driver as one transfer -- into:
//
//   psram-aligned    heap_caps_aligned_alloc(64, ..., SPIRAM): DMA straight
//                    into PSRAM, the failing case
//   psram-unaligned  PSRAM, one byte past a heap block: not DMA-alignable, so
//                    the driver goes through its bounce-buffer path
//   internal         64-byte-aligned internal RAM (DMA capable), small sizes
//                    only, as internal RAM is scarce

#include "sdtest.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define TEST_DIR     "/sd/idf6test"
#define PATTERN_PATH TEST_DIR "/pattern.bin"
#define PATTERN_KEY  0x5A3C96E1u
#define STAGE_SIZE   4096
#define MAX_RUNS     6
#define RUN_GAP      16     // bad regions closer than this count as one run

static sdtest_log_fn s_log;

static void report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s_log(line);
}

static inline uint8_t expected_byte(uint64_t off) {
    uint32_t word = (uint32_t)(off & ~3ULL) ^ PATTERN_KEY;
    return (uint8_t)(word >> (8 * (off & 3)));
}

static void fill_expected(uint8_t* buf, uint64_t off, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = expected_byte(off + i);
}

static uint64_t pattern_size(void) {
    return (uint64_t)CONFIG_IDF6TEST_PATTERN_SIZE_MB * 1024 * 1024;
}

// ---- Pattern file -----------------------------------------------------------

// Read the whole file through the small internal-RAM buffer and compare.
static bool verify_pattern_staged(void) {
    uint8_t* got  = heap_caps_malloc(STAGE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t* want = heap_caps_malloc(STAGE_SIZE, MALLOC_CAP_INTERNAL);
    FILE*    f    = fopen(PATTERN_PATH, "rb");
    bool     ok   = got && want && f;
    uint64_t off  = 0;
    if (f) setvbuf(f, NULL, _IONBF, 0);
    while (ok && off < pattern_size()) {
        size_t n = fread(got, 1, STAGE_SIZE, f);
        if (n != STAGE_SIZE) {
            report("  baseline: short read at %" PRIu64 " (%u bytes, errno %d)", off, (unsigned)n, errno);
            ok = false;
            break;
        }
        fill_expected(want, off, STAGE_SIZE);
        if (memcmp(got, want, STAGE_SIZE) != 0) {
            report("  baseline: mismatch in the 4 KB block at %" PRIu64, off);
            ok = false;
            break;
        }
        off += STAGE_SIZE;
    }
    if (f) fclose(f);
    free(got);
    free(want);
    return ok;
}

static bool write_pattern(void) {
    uint8_t* buf = heap_caps_malloc(STAGE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    FILE*    f   = fopen(PATTERN_PATH, "wb");
    bool     ok  = buf && f;
    if (f) setvbuf(f, NULL, _IONBF, 0);
    for (uint64_t off = 0; ok && off < pattern_size(); off += STAGE_SIZE) {
        fill_expected(buf, off, STAGE_SIZE);
        if (fwrite(buf, 1, STAGE_SIZE, f) != STAGE_SIZE) {
            report("  write error at %" PRIu64 " (errno %d)", off, errno);
            ok = false;
        }
    }
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    return ok;
}

static bool prepare_pattern(void) {
    mkdir(TEST_DIR, 0777);
    struct stat st;
    if (stat(PATTERN_PATH, &st) == 0 && (uint64_t)st.st_size == pattern_size()) {
        report("Pattern file present (%u MB), verifying through 4 KB internal reads...",
             CONFIG_IDF6TEST_PATTERN_SIZE_MB);
        if (verify_pattern_staged()) {
            report("  baseline OK");
            return true;
        }
        report("  baseline failed; writing the pattern file again");
    }
    report("Writing %u MB pattern file through 4 KB internal writes...", CONFIG_IDF6TEST_PATTERN_SIZE_MB);
    int64_t t0 = esp_timer_get_time();
    if (!write_pattern()) {
        report("  FAILED to write the pattern file");
        return false;
    }
    report("  written in %lld ms, verifying...", (esp_timer_get_time() - t0) / 1000);
    if (!verify_pattern_staged()) {
        report("  BASELINE BAD: even 4 KB internal reads do not return the pattern");
        return false;
    }
    report("  baseline OK");
    return true;
}

// ---- Analysis ---------------------------------------------------------------

typedef struct {
    size_t start;   // relative to the buffer
    size_t len;
} run_t;

static const char* describe_run(const uint8_t* buf, uint64_t file_off, const run_t* r, char* out, size_t outlen) {
    bool all_fill = true, all_zero = true;
    for (size_t i = 0; i < r->len; i++) {
        if (buf[r->start + i] != 0xEE) all_fill = false;
        if (buf[r->start + i] != 0x00) all_zero = false;
    }
    if (all_fill) return "untouched (0xEE fill)";
    if (all_zero) return "zeros";
    // Decode the first whole word that sits at a 4-byte file offset.
    for (size_t j = 0; j + 4 <= r->len; j++) {
        uint64_t at = file_off + r->start + j;
        if (at % 4) continue;
        const uint8_t* p = buf + r->start + j;
        uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t src  = word ^ PATTERN_KEY;
        if ((src % 4) == 0 && src < pattern_size()) {
            snprintf(out, outlen, "data from file offset %" PRIu32 " (%+lld bytes)", src, (long long)src - (long long)at);
            return out;
        }
        break;
    }
    return "unrecognised";
}

static const char* granularity(const run_t* runs, int nruns, uint64_t file_off) {
    static const size_t sizes[]  = {4096, 512, 64, 4};
    static const char*  names[]  = {"4096-byte", "512-byte", "64-byte", "4-byte"};
    for (int g = 0; g < 4; g++) {
        bool fits = true;
        for (int i = 0; i < nruns; i++) {
            if (((file_off + runs[i].start) % sizes[g]) || (runs[i].len % sizes[g])) fits = false;
        }
        if (fits) return names[g];
    }
    return "irregular";
}

// Returns the number of bad bytes and logs the runs.
static size_t analyse(const uint8_t* buf, uint64_t file_off, size_t size) {
    run_t  runs[MAX_RUNS];
    int    nruns = 0, total_runs = 0;
    size_t bad = 0;
    size_t i = 0;
    while (i < size) {
        if (buf[i] == expected_byte(file_off + i)) {
            i++;
            continue;
        }
        size_t start = i, last_bad = i, good_streak = 0;
        for (; i < size; i++) {
            if (buf[i] != expected_byte(file_off + i)) {
                bad++;
                last_bad = i;
                good_streak = 0;
            } else if (++good_streak >= RUN_GAP) {
                break;
            }
        }
        if (nruns < MAX_RUNS) {
            runs[nruns].start = start;
            runs[nruns].len   = last_bad - start + 1;
            nruns++;
        }
        total_runs++;
    }
    if (bad) {
        report("    %u bad bytes in %d run(s), %s granularity%s", (unsigned)bad, total_runs,
             granularity(runs, nruns, file_off), total_runs > nruns ? " (first runs only)" : "");
        for (int r = 0; r < nruns; r++) {
            char desc[96];
            report("    +%u len %u: %s", (unsigned)runs[r].start, (unsigned)runs[r].len,
                 describe_run(buf, file_off, &runs[r], desc, sizeof(desc)));
        }
    }
    return bad;
}

// ---- Read cases -------------------------------------------------------------

typedef enum { DEST_PSRAM_ALIGNED = 0, DEST_PSRAM_UNALIGNED, DEST_INTERNAL, DEST_COUNT } dest_t;
static const char* const dest_names[DEST_COUNT] = {"psram-aligned", "psram-unaligned", "internal"};

static const size_t   sizes[]   = {4096, 8192, 12288, 16384, 20480, 32768, 65536, 131072, 262144, 1048576, 4194304};
static const uint64_t offsets[] = {0, 1048576, 1048576 + 123};
#define NSIZES   (sizeof(sizes) / sizeof(sizes[0]))
#define NOFFSETS (sizeof(offsets) / sizeof(offsets[0]))
#define REPEATS  2
#define INTERNAL_MAX 65536

// Returns 1 if good, 0 if bad, -1 if skipped.
static int run_case(dest_t dest, size_t size, uint64_t off, int rep) {
    void*    block = NULL;
    uint8_t* buf   = NULL;
    switch (dest) {
        case DEST_PSRAM_ALIGNED:
            block = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
            buf   = block;
            break;
        case DEST_PSRAM_UNALIGNED:
            block = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
            buf   = block ? (uint8_t*)block + 1 : NULL;
            break;
        case DEST_INTERNAL:
            if (size > INTERNAL_MAX) return -1;
            block = heap_caps_aligned_alloc(64, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            buf   = block;
            break;
        default:
            return -1;
    }
    if (!buf) {
        report("  %-15s %7u @%-8" PRIu64 " #%d: no memory, skipped", dest_names[dest], (unsigned)size, off, rep);
        return -1;
    }
    memset(buf, 0xEE, size);

    FILE* f = fopen(PATTERN_PATH, "rb");
    if (!f) {
        heap_caps_free(block);
        report("  cannot open the pattern file (errno %d)", errno);
        return 0;
    }
    setvbuf(f, NULL, _IONBF, 0);
    int     seek = fseek(f, (long)off, SEEK_SET);
    int64_t t0   = esp_timer_get_time();
    size_t  n    = seek == 0 ? fread(buf, 1, size, f) : 0;
    int64_t us   = esp_timer_get_time() - t0;
    int     err  = errno;
    fclose(f);

    int result;
    if (n != size) {
        report("  %-15s %7u @%-8" PRIu64 " #%d: SHORT READ %u bytes (errno %d)", dest_names[dest], (unsigned)size, off,
             rep, (unsigned)n, err);
        result = 0;
    } else {
        unsigned kbps = us > 0 ? (unsigned)((uint64_t)size * 1000000 / (uint64_t)us / 1024) : 0;
        // Log the verdict line first, then the details analyse() prints.
        size_t bad = 0;
        for (size_t i = 0; i < size && !bad; i++) {
            if (buf[i] != expected_byte(off + i)) bad = 1;
        }
        report("  %-15s %7u @%-8" PRIu64 " #%d: %s  %u KB/s", dest_names[dest], (unsigned)size, off, rep,
             bad ? "BAD" : "ok", kbps);
        if (bad) analyse(buf, off, size);
        result = bad ? 0 : 1;
    }
    heap_caps_free(block);
    return result;
}

bool sdtest_run(sdtest_log_fn log) {
    s_log = log;
    if (!prepare_pattern()) return false;

    int ok[DEST_COUNT][NSIZES]    = {0};
    int total[DEST_COUNT][NSIZES] = {0};
    bool all_ok = true;

    for (int d = 0; d < DEST_COUNT; d++) {
        report("Reads into %s:", dest_names[d]);
        for (size_t s = 0; s < NSIZES; s++) {
            for (size_t o = 0; o < NOFFSETS; o++) {
                if (offsets[o] + sizes[s] > pattern_size()) continue;
                for (int r = 1; r <= REPEATS; r++) {
                    int res = run_case((dest_t)d, sizes[s], offsets[o], r);
                    if (res < 0) continue;
                    total[d][s]++;
                    ok[d][s] += res;
                    if (!res) all_ok = false;
                }
            }
        }
    }

    report("Summary (good/total):");
    char line[256];
    int  pos = snprintf(line, sizeof(line), "  %-15s", "size");
    for (size_t s = 0; s < NSIZES; s++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %6uK", (unsigned)(sizes[s] / 1024));
    }
    report("%s", line);
    for (int d = 0; d < DEST_COUNT; d++) {
        pos = snprintf(line, sizeof(line), "  %-15s", dest_names[d]);
        for (size_t s = 0; s < NSIZES; s++) {
            if (total[d][s]) {
                pos += snprintf(line + pos, sizeof(line) - pos, " %3d/%-3d", ok[d][s], total[d][s]);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "    -   ");
            }
        }
        report("%s", line);
    }
    report(all_ok ? "RESULT: all reads correct" : "RESULT: CORRUPTED READS FOUND");
    return all_ok;
}
