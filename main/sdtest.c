// SD card read test for the ESP-IDF 6 corruption described in
// idf6_sd_fuckup.md.
//
// The pattern file makes corruption self-describing: the 4-byte word at file
// offset `o` holds mix(o / 4 ^ PATTERN_KEY), where mix() is an invertible 32-bit
// hash. A word that lands in the wrong place therefore says where it was meant
// to go, and because every byte of neighbouring words differs, bad bytes and
// runs are counted exactly. The report can tell whole blocks that were shifted
// or duplicated from zeros, untouched memory or garbage.
//
// The file is written and baseline-verified through a 4 KB internal-RAM
// buffer, the path known to work (graceloader's workaround).
//
// Every read case is described by:
//
//   dest    where the bytes go
//             psram      heap_caps_aligned_alloc(64, SPIRAM): DMA straight into PSRAM
//             psram+1    PSRAM, one byte past a heap block: not DMA-alignable, so the
//                        driver bounces through dma_aligned_buffer or a temp buffer
//             page       aligned_alloc(4096) as kbelf's segment allocator does; lands
//                        in PSRAM for sizes above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
//             internal   64-byte-aligned internal DMA RAM (sizes <= 64 KB only)
//   io      stdio buffering of the FILE
//             unbuf      _IONBF: FatFs gets the whole request in one f_read
//             default    newlib's default buffer
//             fast       setvbuf() with an internal DMA buffer of `vbuf` bytes, as
//                        graceloader's fastopen()
//   method  how the caller reads
//             direct     one fread() of the whole size into dest
//             staged     fread() `chunk` bytes at a time into internal DMA RAM, memcpy
//             chunked    fread() `chunk` bytes at a time straight into dest
//   dmabuf  host.dma_aligned_buffer set (1, as graceloader) or NULL (0)
//
// Suites and their options (all key=value, all optional):
//
//   smoke       a few direct PSRAM reads                selftest=1
//   matrix      dest x size x offset x repeat           reps io dmabuf maxsize
//   stress      one case, many repeats                  dest size off reps io method chunk vbuf dmabuf
//   grace       graceloader's load pattern; with file=  reps dmabuf file
//               also loads that ELF's PT_LOAD segments
//   workaround  correctness and speed of read methods   reps
//
// selftest=1 (any suite) plants a copied 4 KB block into the first case of at
// least 16 KB, to prove the analysis reports it.

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
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "report.h"
#include "sdcard.h"
#include "sdkconfig.h"

#define TEST_DIR     "/sd/idf6test"
#define PATTERN_PATH TEST_DIR "/pattern.bin"
#define PATTERN_KEY  0x5A3C96E1u
#define STAGE_SIZE   4096
#define MAX_RUNS     10
#define RUN_GAP      16  // bad regions closer than this count as one run
#define INTERNAL_MAX 65536

static sdtest_log_fn s_log;
static bool          s_pattern_ok = false;
static bool          s_selftest   = false;

static void text(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void text(const char* fmt, ...) {
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s_log(line);
}

// ---- Arguments --------------------------------------------------------------

bool sdtest_arg_str(const char* args, const char* key, char* out, size_t outlen) {
    size_t klen = strlen(key);
    for (const char* p = args; p && *p;) {
        while (*p == ' ') p++;
        const char* end = strchr(p, ' ');
        size_t      len = end ? (size_t)(end - p) : strlen(p);
        if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = len - klen - 1;
            if (vlen >= outlen) vlen = outlen - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

long sdtest_arg_int(const char* args, const char* key, long def) {
    char buf[32];
    if (!sdtest_arg_str(args, key, buf, sizeof(buf))) return def;
    return strtol(buf, NULL, 0);
}

// ---- Pattern file -----------------------------------------------------------

// lowbias32 and its inverse (Chris Wellons, hash-prospector).
static inline uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline uint32_t unmix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x43021123u;
    x ^= (x >> 15) ^ (x >> 30);
    x *= 0x1d69e2a5u;
    x ^= x >> 16;
    return x;
}

static inline uint8_t expected_byte(uint64_t off) {
    uint32_t word = mix((uint32_t)(off >> 2) ^ PATTERN_KEY);
    return (uint8_t)(word >> (8 * (off & 3)));
}

static void fill_expected(uint8_t* buf, uint64_t off, size_t len) {
    for (size_t i = 0; i < len; i++) buf[i] = expected_byte(off + i);
}

static uint64_t pattern_size(void) {
    return (uint64_t)CONFIG_IDF6TEST_PATTERN_SIZE_MB * 1024 * 1024;
}

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
            text("  baseline: short read at %" PRIu64 " (%u bytes, errno %d)", off, (unsigned)n, errno);
            ok = false;
            break;
        }
        fill_expected(want, off, STAGE_SIZE);
        if (memcmp(got, want, STAGE_SIZE) != 0) {
            text("  baseline: mismatch in the 4 KB block at %" PRIu64, off);
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
            text("  write error at %" PRIu64 " (errno %d)", off, errno);
            ok = false;
        }
    }
    if (f && fclose(f) != 0) ok = false;
    free(buf);
    return ok;
}

static bool prepare_pattern(void) {
    if (s_pattern_ok) return true;
    mkdir(TEST_DIR, 0777);
    struct stat st;
    int64_t     t0 = esp_timer_get_time();
    if (stat(PATTERN_PATH, &st) == 0 && (uint64_t)st.st_size == pattern_size()) {
        text("Pattern file present (%u MB), verifying through 4 KB internal reads...", CONFIG_IDF6TEST_PATTERN_SIZE_MB);
        if (verify_pattern_staged()) {
            text("  baseline OK (%lld ms)", (esp_timer_get_time() - t0) / 1000);
            report_emitf("PATTERN", "{\"t\":\"pattern\",\"written\":false,\"baseline\":true}");
            s_pattern_ok = true;
            return true;
        }
        text("  baseline failed; writing the pattern file again");
    }
    text("Writing %u MB pattern file through 4 KB internal writes...", CONFIG_IDF6TEST_PATTERN_SIZE_MB);
    if (!write_pattern()) {
        text("  FAILED to write the pattern file");
        report_emitf("PATTERN", "{\"t\":\"pattern\",\"written\":false,\"baseline\":false}");
        return false;
    }
    text("  written in %lld ms, verifying...", (esp_timer_get_time() - t0) / 1000);
    s_pattern_ok = verify_pattern_staged();
    text(s_pattern_ok ? "  baseline OK" : "  BASELINE BAD: even 4 KB internal reads do not return the pattern");
    report_emitf("PATTERN", "{\"t\":\"pattern\",\"written\":true,\"baseline\":%s}", s_pattern_ok ? "true" : "false");
    return s_pattern_ok;
}

// ---- Analysis ---------------------------------------------------------------

typedef struct {
    size_t start;  // relative to the buffer
    size_t len;
} run_t;

typedef struct {
    size_t bad;         // bytes that differ from the pattern
    size_t bad_ee;      // ... of which still hold the 0xEE fill
    size_t bad_zero;    // ... of which are zero
    int    total_runs;  // runs of bad bytes (RUN_GAP merges close ones)
    int    nruns;       // runs recorded below
    run_t  runs[MAX_RUNS];
} analysis_t;

// `fill` is the byte the buffer held before the read.
static const char* describe_run(const uint8_t* buf, uint64_t file_off, const run_t* r, uint8_t fill, char* out,
                                size_t outlen) {
    bool all_fill = true, all_zero = true;
    for (size_t i = 0; i < r->len; i++) {
        if (buf[r->start + i] != fill) all_fill = false;
        if (buf[r->start + i] != 0x00) all_zero = false;
    }
    if (all_fill && fill != 0) return "untouched fill";
    if (all_zero) return fill == 0 ? "zeros (= untouched fill)" : "zeros";
    // Decode the first whole word that sits at a 4-byte file offset.
    for (size_t j = 0; j + 4 <= r->len; j++) {
        uint64_t at = file_off + r->start + j;
        if (at % 4) continue;
        const uint8_t* p    = buf + r->start + j;
        uint32_t       word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint64_t       src  = ((uint64_t)(unmix(word) ^ PATTERN_KEY)) << 2;
        if (src < pattern_size()) {
            snprintf(out, outlen, "data from file offset %" PRIu64 " (%+lld)", src, (long long)src - (long long)at);
            return out;
        }
        break;
    }
    return "unrecognised";
}

static const char* granularity(const analysis_t* a, uint64_t file_off) {
    static const size_t sizes[] = {4096, 512, 64, 4};
    static const char*  names[] = {"4096", "512", "64", "4"};
    for (int g = 0; g < 4; g++) {
        bool fits = true;
        for (int i = 0; i < a->nruns; i++) {
            if (((file_off + a->runs[i].start) % sizes[g]) || (a->runs[i].len % sizes[g])) fits = false;
        }
        if (fits) return names[g];
    }
    return "irregular";
}

// Works in whole pattern words (aligned to the file offset), so a run's start
// and length reflect the corruption's real granularity even where a wrong word
// happens to share a byte with the right one.
static void analyse(const uint8_t* buf, uint64_t file_off, size_t size, uint8_t fill, analysis_t* a) {
    memset(a, 0, sizeof(*a));
    bool   in_run = false;
    size_t start = 0, end = 0, good = 0;
    for (size_t i = 0; i < size;) {
        size_t next    = i + (4 - ((file_off + i) & 3));
        if (next > size) next = size;
        bool word_bad = false;
        for (size_t k = i; k < next; k++) {
            if (buf[k] != expected_byte(file_off + k)) {
                word_bad = true;
                a->bad++;
                if (buf[k] == fill) a->bad_ee++;
                if (buf[k] == 0) a->bad_zero++;
            }
        }
        if (word_bad) {
            if (!in_run) {
                in_run = true;
                start  = i;
            }
            end  = next;
            good = 0;
        } else if (in_run && (good += next - i) >= RUN_GAP) {
            if (a->nruns < MAX_RUNS) a->runs[a->nruns++] = (run_t){start, end - start};
            a->total_runs++;
            in_run = false;
        }
        i = next;
    }
    if (in_run) {
        if (a->nruns < MAX_RUNS) a->runs[a->nruns++] = (run_t){start, end - start};
        a->total_runs++;
    }
}

// ---- Read cases -------------------------------------------------------------

typedef enum { DEST_PSRAM = 0, DEST_PSRAM_UNALIGNED, DEST_PAGE, DEST_INTERNAL, DEST_COUNT } dest_t;
static const char* const dest_names[DEST_COUNT] = {"psram", "psram+1", "page", "internal"};

typedef enum { IO_UNBUF = 0, IO_DEFAULT, IO_FAST, IO_COUNT } io_t;
static const char* const io_names[IO_COUNT] = {"unbuf", "default", "fast"};

typedef enum { METHOD_DIRECT = 0, METHOD_STAGED, METHOD_CHUNKED, METHOD_COUNT } method_t;
static const char* const method_names[METHOD_COUNT] = {"direct", "staged", "chunked"};

typedef struct {
    const char* suite;
    dest_t      dest;
    io_t        io;
    method_t    method;
    size_t      chunk;  // staged/chunked
    size_t      vbuf;   // io=fast
    bool        dmabuf;
    size_t      size;
    uint64_t    off;
    int         rep;
    uint8_t     fill;  // 0xEE normally, 0 to mimic kbelf's memset
} case_t;

typedef struct {
    int      cases, good, bad, skipped;
    uint64_t bytes, us;  // over the good and bad cases
} tally_t;

static int lookup(const char* const* names, int count, const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) return i;
    }
    return -1;
}

static void* alloc_dest(dest_t dest, size_t size, uint8_t** buf) {
    void* block = NULL;
    *buf        = NULL;
    switch (dest) {
        case DEST_PSRAM:
            block = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM);
            *buf  = block;
            break;
        case DEST_PSRAM_UNALIGNED:
            block = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
            *buf  = block ? (uint8_t*)block + 1 : NULL;
            break;
        case DEST_PAGE:
            block = aligned_alloc(4096, size);
            *buf  = block;
            break;
        case DEST_INTERNAL:
            if (size <= INTERNAL_MAX) {
                block = heap_caps_aligned_alloc(64, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
                *buf  = block;
            }
            break;
        default:
            break;
    }
    return block;
}

// Performs the read described by `c` into `buf`. Returns bytes read.
static size_t do_read(const case_t* c, FILE* f, uint8_t* buf, int* err) {
    size_t n = 0;
    *err     = 0;
    if (c->method == METHOD_DIRECT) {
        n = fread(buf, 1, c->size, f);
    } else {
        uint8_t* stage = NULL;
        if (c->method == METHOD_STAGED) {
            stage = heap_caps_aligned_alloc(64, c->chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!stage) {
                *err = ENOMEM;
                return 0;
            }
        }
        while (n < c->size) {
            size_t want = c->size - n < c->chunk ? c->size - n : c->chunk;
            size_t got  = fread(stage ? stage : buf + n, 1, want, f);
            if (stage) memcpy(buf + n, stage, got);
            n += got;
            if (got < want) break;
        }
        heap_caps_free(stage);
    }
    if (n != c->size) *err = errno;
    return n;
}

static void case_label(const case_t* c, char* out, size_t outlen) {
    char method[24];
    if (c->method == METHOD_DIRECT) {
        snprintf(method, sizeof(method), "direct");
    } else {
        snprintf(method, sizeof(method), "%s%uK", method_names[c->method], (unsigned)(c->chunk / 1024));
    }
    snprintf(out, outlen, "%-8s %-7s %-10s db%d %7u @%-8" PRIu64 " #%d", dest_names[c->dest], io_names[c->io], method,
             c->dmabuf, (unsigned)c->size, c->off, c->rep);
}

// Returns 1 if good, 0 if bad, -1 if skipped. Always emits a CASE record.
static int run_case(const case_t* c, tally_t* t) {
    char label[96];
    case_label(c, label, sizeof(label));

    uint8_t* buf   = NULL;
    void*    block = alloc_dest(c->dest, c->size, &buf);
    void*    vbuf  = NULL;
    FILE*    f     = NULL;
    int      result;

    if (!buf) {
        text("  %s: no memory, skipped", label);
        result = -1;
        goto emit;
    }
    if (c->io == IO_FAST) {
        vbuf = heap_caps_malloc(c->vbuf, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!vbuf) {
            text("  %s: no memory for the stdio buffer, skipped", label);
            result = -1;
            goto emit;
        }
    }
    memset(buf, c->fill, c->size);
    sdcard_set_dma_aligned_buffer(c->dmabuf);

    f = fopen(PATTERN_PATH, "rb");
    if (!f) {
        text("  cannot open the pattern file (errno %d)", errno);
        result = -1;
        goto emit;
    }
    if (c->io == IO_UNBUF) setvbuf(f, NULL, _IONBF, 0);
    if (c->io == IO_FAST) setvbuf(f, vbuf, _IOFBF, c->vbuf);

    int     err  = 0;
    int     seek = fseek(f, (long)c->off, SEEK_SET);
    int64_t t0   = esp_timer_get_time();
    size_t  n    = seek == 0 ? do_read(c, f, buf, &err) : 0;
    int64_t us   = esp_timer_get_time() - t0;
    fclose(f);
    f = NULL;
    sdcard_set_dma_aligned_buffer(true);

    if (s_selftest && c->size >= 16384 && n == c->size) {
        memcpy(buf + 4096, buf + 12288, 4096);
        s_selftest = false;
        text("  (selftest: planted a copy of +12288 at +4096 in the next case)");
    }

    analysis_t a;
    analyse(buf, c->off, n, c->fill, &a);
    bool short_read = n != c->size;
    result          = (!short_read && a.bad == 0) ? 1 : 0;
    unsigned kbps   = us > 0 ? (unsigned)((uint64_t)n * 1000000 / (uint64_t)us / 1024) : 0;
    t->bytes += n;
    t->us += (uint64_t)us;

    if (short_read) {
        text("  %s: SHORT READ %u bytes (errno %d)", label, (unsigned)n, err);
    } else {
        text("  %s: %s  %u KB/s", label, result ? "ok" : "BAD", kbps);
    }
    char detail[REPORT_JSON_MAX / 2];
    int  dpos = 0;
    detail[0] = '\0';
    if (a.bad) {
        text("    %u bad bytes (%u fill, %u zero) in %d run(s), %s-byte granularity%s", (unsigned)a.bad,
             (unsigned)a.bad_ee, (unsigned)a.bad_zero, a.total_runs, granularity(&a, c->off),
             a.total_runs > a.nruns ? " (first runs only)" : "");
        for (int r = 0; r < a.nruns; r++) {
            char        desc[80];
            const char* d = describe_run(buf, c->off, &a.runs[r], c->fill, desc, sizeof(desc));
            text("    +%u len %u: %s", (unsigned)a.runs[r].start, (unsigned)a.runs[r].len, d);
            if (dpos < (int)sizeof(detail) - 1) {
                dpos += snprintf(detail + dpos, sizeof(detail) - dpos, "%s{\"s\":%u,\"l\":%u,\"d\":\"%s\"}",
                                 r ? "," : "", (unsigned)a.runs[r].start, (unsigned)a.runs[r].len, d);
            }
        }
    }

    report_emitf("CASE",
                 "{\"t\":\"case\",\"suite\":\"%s\",\"dest\":\"%s\",\"io\":\"%s\",\"method\":\"%s\",\"chunk\":%u,"
                 "\"vbuf\":%u,\"dmabuf\":%d,\"size\":%u,\"off\":%" PRIu64 ",\"rep\":%d,\"addr\":\"%p\","
                 "\"ok\":%s,\"n\":%u,\"errno\":%d,\"bad\":%u,\"bad_fill\":%u,\"bad_zero\":%u,\"runs\":%d,"
                 "\"gran\":\"%s\",\"us\":%lld,\"kbps\":%u,\"detail\":[%s]}",
                 c->suite, dest_names[c->dest], io_names[c->io], method_names[c->method], (unsigned)c->chunk,
                 (unsigned)(c->io == IO_FAST ? c->vbuf : 0), c->dmabuf, (unsigned)c->size, c->off, c->rep, buf,
                 result ? "true" : "false", (unsigned)n, err, (unsigned)a.bad, (unsigned)a.bad_ee,
                 (unsigned)a.bad_zero, a.total_runs, a.bad ? granularity(&a, c->off) : "", us, kbps, detail);
    t->cases++;
    if (result) {
        t->good++;
    } else {
        t->bad++;
    }
    heap_caps_free(vbuf);
    heap_caps_free(block);
    return result;

emit:
    report_emitf("CASE",
                 "{\"t\":\"case\",\"suite\":\"%s\",\"dest\":\"%s\",\"io\":\"%s\",\"method\":\"%s\",\"chunk\":%u,"
                 "\"dmabuf\":%d,\"size\":%u,\"off\":%" PRIu64 ",\"rep\":%d,\"skipped\":true}",
                 c->suite, dest_names[c->dest], io_names[c->io], method_names[c->method], (unsigned)c->chunk,
                 c->dmabuf, (unsigned)c->size, c->off, c->rep);
    if (f) fclose(f);
    heap_caps_free(vbuf);
    heap_caps_free(block);
    t->skipped++;
    return result;
}

static case_t default_case(const char* suite, const char* args) {
    case_t c = {
        .suite  = suite,
        .dest   = DEST_PSRAM,
        .io     = IO_UNBUF,
        .method = METHOD_DIRECT,
        .chunk  = 4096,
        .vbuf   = 8192,
        .dmabuf = sdtest_arg_int(args, "dmabuf", 1) != 0,
        .size   = 65536,
        .off    = 0,
        .rep    = 1,
        .fill   = 0xEE,
    };
    char name[24];
    int  idx;
    if (sdtest_arg_str(args, "io", name, sizeof(name)) && (idx = lookup(io_names, IO_COUNT, name)) >= 0) c.io = idx;
    if (sdtest_arg_str(args, "dest", name, sizeof(name)) && (idx = lookup(dest_names, DEST_COUNT, name)) >= 0)
        c.dest = idx;
    if (sdtest_arg_str(args, "method", name, sizeof(name)) && (idx = lookup(method_names, METHOD_COUNT, name)) >= 0)
        c.method = idx;
    c.chunk = sdtest_arg_int(args, "chunk", c.chunk);
    c.vbuf  = sdtest_arg_int(args, "vbuf", c.vbuf);
    c.size  = sdtest_arg_int(args, "size", c.size);
    c.off   = sdtest_arg_int(args, "off", c.off);
    c.fill  = sdtest_arg_int(args, "fill", c.fill);
    return c;
}

// ---- Suites -----------------------------------------------------------------

static void suite_smoke(const char* args, tally_t* t) {
    static const size_t sizes[] = {4096, 65536, 262144};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        case_t c = default_case("smoke", args);
        c.size   = sizes[s];
        run_case(&c, t);
    }
}

static const size_t matrix_sizes[] = {4096,   8192,   12288,  16384,   20480,  32768,
                                      65536,  131072, 150000, 262144, 1048576, 4194304};
#define MATRIX_NSIZES (sizeof(matrix_sizes) / sizeof(matrix_sizes[0]))

static void suite_matrix(const char* args, tally_t* t) {
    static const uint64_t offsets[] = {0, 1048576, 1048576 + 123};
    int                   reps      = sdtest_arg_int(args, "reps", 2);
    size_t                maxsize   = sdtest_arg_int(args, "maxsize", 4194304);
    int                   ok[DEST_COUNT][MATRIX_NSIZES]    = {0};
    int                   total[DEST_COUNT][MATRIX_NSIZES] = {0};

    for (int d = 0; d < DEST_COUNT; d++) {
        text("Reads into %s:", dest_names[d]);
        for (size_t s = 0; s < MATRIX_NSIZES; s++) {
            if (matrix_sizes[s] > maxsize) continue;
            for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
                if (offsets[o] + matrix_sizes[s] > pattern_size()) continue;
                for (int r = 1; r <= reps; r++) {
                    case_t c = default_case("matrix", args);
                    c.dest   = d;
                    c.size   = matrix_sizes[s];
                    c.off    = offsets[o];
                    c.rep    = r;
                    int res  = run_case(&c, t);
                    if (res < 0) continue;
                    total[d][s]++;
                    ok[d][s] += res;
                }
            }
        }
    }

    text("Summary (good/total):");
    char line[256];
    int  pos = snprintf(line, sizeof(line), "  %-9s", "size K");
    for (size_t s = 0; s < MATRIX_NSIZES; s++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %6u", (unsigned)(matrix_sizes[s] / 1024));
    }
    text("%s", line);
    for (int d = 0; d < DEST_COUNT; d++) {
        pos = snprintf(line, sizeof(line), "  %-9s", dest_names[d]);
        for (size_t s = 0; s < MATRIX_NSIZES; s++) {
            if (total[d][s]) {
                pos += snprintf(line + pos, sizeof(line) - pos, " %3d/%-2d", ok[d][s], total[d][s]);
            } else {
                pos += snprintf(line + pos, sizeof(line) - pos, "      -");
            }
        }
        text("%s", line);
    }
}

static void suite_stress(const char* args, tally_t* t) {
    int reps = sdtest_arg_int(args, "reps", 20);
    for (int r = 1; r <= reps; r++) {
        case_t c = default_case("stress", args);
        c.rep    = r;
        run_case(&c, t);
    }
}

// ELF32 structures, just what the segment loader needs.
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} elf32_phdr_t;

// Load an ELF's PT_LOAD segments the way kbelf does before graceloader's
// workaround: fopen() (default buffering), aligned_alloc() over the whole span,
// memset of the BSS tail, then one fread() per segment. Each segment is then
// re-read through the known-good staged path and compared.
static void grace_elf(const char* path, const char* args, int rep, tally_t* t) {
    bool          dmabuf = sdtest_arg_int(args, "dmabuf", 1) != 0;
    FILE*         f      = fopen(path, "rb");
    elf32_ehdr_t  eh;
    elf32_phdr_t* ph     = NULL;
    void*         memory = NULL;
    uint8_t*      stage  = NULL;
    if (!f) {
        text("  cannot open %s (errno %d)", path, errno);
        t->skipped++;
        return;
    }
    if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 ||
        eh.e_phentsize != sizeof(elf32_phdr_t)) {
        text("  %s is not an ELF32 file", path);
        t->skipped++;
        goto out;
    }
    ph = calloc(eh.e_phnum, sizeof(*ph));
    if (!ph || fseek(f, eh.e_phoff, SEEK_SET) != 0 || fread(ph, sizeof(*ph), eh.e_phnum, f) != eh.e_phnum) {
        text("  cannot read the program headers");
        t->skipped++;
        goto out;
    }
    size_t min_va = SIZE_MAX, max_va = 0, align = 16;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != 1) continue;
        if (align < ph[i].p_align) align = ph[i].p_align;
        if (min_va > ph[i].p_vaddr) min_va = ph[i].p_vaddr;
        if (max_va < ph[i].p_vaddr + ph[i].p_memsz) max_va = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (max_va <= min_va) {
        text("  no PT_LOAD segments");
        t->skipped++;
        goto out;
    }
    memory = aligned_alloc(align, max_va - min_va);
    stage  = heap_caps_malloc(STAGE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!memory || !stage) {
        text("  no memory for %u bytes", (unsigned)(max_va - min_va));
        t->skipped++;
        goto out;
    }
    sdcard_set_dma_aligned_buffer(dmabuf);
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != 1 || ph[i].p_filesz == 0) continue;
        uint8_t* laddr = (uint8_t*)memory + (ph[i].p_vaddr - min_va);
        memset(laddr + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
        fseek(f, ph[i].p_offset, SEEK_SET);
        int64_t t0 = esp_timer_get_time();
        size_t  n  = fread(laddr, 1, ph[i].p_filesz, f);
        int64_t us = esp_timer_get_time() - t0;

        // Reference: the same bytes through 4 KB internal reads.
        FILE* g = fopen(path, "rb");
        if (!g) break;
        setvbuf(g, NULL, _IONBF, 0);
        fseek(g, ph[i].p_offset, SEEK_SET);
        size_t bad = 0, first_bad = SIZE_MAX, last_bad = 0;
        for (size_t pos = 0; pos < n;) {
            size_t want = n - pos < STAGE_SIZE ? n - pos : STAGE_SIZE;
            size_t got  = fread(stage, 1, want, g);
            if (got != want) break;
            for (size_t j = 0; j < got; j++) {
                if (laddr[pos + j] != stage[j]) {
                    bad++;
                    if (first_bad == SIZE_MAX) first_bad = pos + j;
                    last_bad = pos + j;
                }
            }
            pos += got;
        }
        fclose(g);

        bool ok = n == ph[i].p_filesz && bad == 0;
        text("  elf seg%d rep%d: %u bytes @%u -> %p: %s (%u bad, first +%d, last +%d) %u KB/s", i, rep,
             (unsigned)ph[i].p_filesz, (unsigned)ph[i].p_offset, laddr, ok ? "ok" : "BAD", (unsigned)bad,
             bad ? (int)first_bad : -1, bad ? (int)last_bad : -1,
             us > 0 ? (unsigned)((uint64_t)n * 1000000 / (uint64_t)us / 1024) : 0);
        report_emitf("CASE",
                     "{\"t\":\"case\",\"suite\":\"grace\",\"dest\":\"elf-seg%d\",\"io\":\"default\","
                     "\"method\":\"direct\",\"dmabuf\":%d,\"size\":%u,\"off\":%u,\"rep\":%d,\"addr\":\"%p\","
                     "\"ok\":%s,\"n\":%u,\"bad\":%u,\"first_bad\":%d,\"last_bad\":%d,\"us\":%lld,\"file\":\"%s\"}",
                     i, dmabuf, (unsigned)ph[i].p_filesz, (unsigned)ph[i].p_offset, rep, laddr, ok ? "true" : "false",
                     (unsigned)n, (unsigned)bad, bad ? (int)first_bad : -1, bad ? (int)last_bad : -1, us, path);
        t->cases++;
        t->bytes += n;
        t->us += us;
        if (ok) {
            t->good++;
        } else {
            t->bad++;
        }
    }
out:
    sdcard_set_dma_aligned_buffer(true);
    free(memory);
    free(stage);
    free(ph);
    fclose(f);
}

static void suite_grace(const char* args, tally_t* t) {
    // Synthetic: sizes and offsets like an app's text segment, kbelf's
    // allocation and memset(0), plain fopen() and graceloader's fastopen().
    static const size_t   sizes[]   = {150000, 153600, 1048576};
    static const uint64_t offsets[] = {52, 4096, 4096 + 52};
    int                   reps      = sdtest_arg_int(args, "reps", 2);
    for (int io = IO_DEFAULT; io <= IO_FAST; io++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
                for (int r = 1; r <= reps; r++) {
                    case_t c = default_case("grace", args);
                    c.dest   = DEST_PAGE;
                    c.io     = io;
                    c.size   = sizes[s];
                    c.off    = offsets[o];
                    c.rep    = r;
                    c.fill   = 0;
                    run_case(&c, t);
                }
            }
        }
    }
    char path[160];
    if (sdtest_arg_str(args, "file", path, sizeof(path))) {
        for (int r = 1; r <= reps; r++) grace_elf(path, args, r, t);
    }
}

static void suite_workaround(const char* args, tally_t* t) {
    typedef struct {
        dest_t   dest;
        io_t     io;
        method_t method;
        size_t   chunk;
        size_t   vbuf;
        bool     dmabuf;
    } method_def_t;
    static const method_def_t methods[] = {
        {DEST_PSRAM,           IO_UNBUF,   METHOD_DIRECT,  0,     0,     true },  // M0 reference
        {DEST_PSRAM,           IO_UNBUF,   METHOD_STAGED,  4096,  0,     true },  // M1 graceloader
        {DEST_PSRAM,           IO_UNBUF,   METHOD_STAGED,  16384, 0,     true },  // M2
        {DEST_PSRAM,           IO_UNBUF,   METHOD_STAGED,  32768, 0,     true },
        {DEST_PSRAM,           IO_UNBUF,   METHOD_STAGED,  65536, 0,     true },
        {DEST_PSRAM,           IO_UNBUF,   METHOD_CHUNKED, 12288, 0,     true },  // M3
        {DEST_PSRAM,           IO_UNBUF,   METHOD_CHUNKED, 16384, 0,     true },
        {DEST_PSRAM_UNALIGNED, IO_UNBUF,   METHOD_DIRECT,  0,     0,     true },  // M4
        {DEST_PSRAM_UNALIGNED, IO_UNBUF,   METHOD_DIRECT,  0,     0,     false},
        {DEST_PSRAM,           IO_FAST,    METHOD_DIRECT,  0,     8192,  true },  // M5
        {DEST_PSRAM,           IO_FAST,    METHOD_DIRECT,  0,     65536, true },
        {DEST_PSRAM,           IO_DEFAULT, METHOD_DIRECT,  0,     0,     true },
    };
    static const size_t sizes[] = {150000, 1048576, 4194304};
    int                 reps    = sdtest_arg_int(args, "reps", 3);
    for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (int r = 1; r <= reps; r++) {
                case_t c = default_case("workaround", args);
                c.dest   = methods[m].dest;
                c.io     = methods[m].io;
                c.method = methods[m].method;
                c.chunk  = methods[m].chunk;
                c.vbuf   = methods[m].vbuf;
                c.dmabuf = methods[m].dmabuf;
                c.size   = sizes[s];
                c.off    = 4096;
                c.rep    = r;
                run_case(&c, t);
            }
        }
    }
}

sdtest_status_t sdtest_run(const char* suite, const char* args, sdtest_log_fn log) {
    s_log      = log;
    s_selftest = sdtest_arg_int(args, "selftest", 0) != 0;
    if (!prepare_pattern()) return SDTEST_ERROR;

    tally_t t = {0};
    if (strcmp(suite, "smoke") == 0) {
        suite_smoke(args, &t);
    } else if (strcmp(suite, "matrix") == 0) {
        suite_matrix(args, &t);
    } else if (strcmp(suite, "stress") == 0) {
        suite_stress(args, &t);
    } else if (strcmp(suite, "grace") == 0) {
        suite_grace(args, &t);
    } else if (strcmp(suite, "workaround") == 0) {
        suite_workaround(args, &t);
    } else {
        text("Unknown suite '%s'", suite);
        return SDTEST_ERROR;
    }

    text("%s: %d cases, %d good, %d BAD, %d skipped", suite, t.cases, t.good, t.bad, t.skipped);
    report_emitf("SUMMARY", "{\"t\":\"summary\",\"suite\":\"%s\",\"cases\":%d,\"good\":%d,\"bad\":%d,\"skipped\":%d}",
                 suite, t.cases, t.good, t.bad, t.skipped);
    text(t.bad ? "RESULT: CORRUPTED READS FOUND" : "RESULT: all reads correct");
    return t.bad ? SDTEST_BAD : SDTEST_OK;
}
