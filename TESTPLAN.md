# IDF 6 SD read corruption: test plan and progress

Background: `idf6_sd_fuckup.md`. Goal: reproduce the large-read-into-PSRAM
corruption seen in tanmatsu-graceloader on ESP-IDF 6.0.2, find which change
removes it, and find the cheapest correct workaround (KB/s).

Conventions:

- One device run = one row in the run log. Raw logs and parsed JSON go to
  `results/` and are committed.
- Every hypothesis verdict links to a results file. A root-cause claim needs
  ≥ 20/20 repeats flipping BAD→OK, confirmed by re-running `base` afterwards.
- Workflow: `make cycle VARIANT=<v> SUITE=<s> [ARGS="..."]` (build, install,
  run, capture). See "How to run" at the end.

## 1. Step checklist

### Phase 0 — housekeeping
- [x] 0.1 Commit the prepared test app as-is (`122f70e`).
- [x] 0.2 Create this document.
- [x] 0.3 sdkconfig diff graceloader vs `base` — see §6.
- [x] 0.4 Plumbing: `mode_badgelink` works from the launcher; `badgelink start` boots the **AppFS** copy, so `install` now also uploads to AppFS (as fonttest).

### Phase 1 — automation
- [x] 1.1 Firmware command console (USB-serial/JTAG driver, PING/RUN/REMOUNT/EXIT, READY banner, idle timeout).
- [x] 1.2 Framed, CRC-checked result records.
- [x] 1.3 `main.c` restructure (no startup delay, throttled redraw).
- [x] 1.4 Suites: smoke, matrix, grace (+ real ELF via `file=`), stress, workaround; `selftest=1`; `wifi=1` connects WiFi before any suite. No `REMOUNT` needed: `dmabuf=0/1` switches `card->host.dma_aligned_buffer` live.
- [x] 1.5 Host scripts: `tools/sdtest.py`, `tools/badgelink_retry.sh`.
- [x] 1.6 Makefile: verified `mode_badgelink`, AppFS install, `testrun`, `cycle`, `cycleall`, generic variant patches.
- [x] 1.7 Two back-to-back unattended cycles OK. Self-test: planted block reported as `+4096 len 4096: data from file offset 12288 (+8192)`, 4096-byte granularity. Pattern switched to an invertible hash per word so byte counts/runs are exact.

### Phase 2 — reproduce on `base`
- [x] 2.1 `base`: `matrix` 258/258 ok, `grace` (+ real `app.so` segments) 38/38 ok, `sweep` over 25 MB of PSRAM addresses 36/36 ok, `grace usbhost=1` ok. **Not reproduced on base.**
- [x] 2.3 Approached graceloader's boot: new variant `graceenv` (no `bsp_device_initialize()`, USB PHY switch, `/int` WL mount, USB HID host — exactly graceloader's `app_main` before the load). **Reproduced** on the first run (see §6 "graceenv reproduction").
- [ ] 2.2 Characterise / bisect which graceenv difference matters (variant `nobsp` + runtime args `usbphy=1 intmount=1 usbhost=1`, `canary` suite).

### Phase 3 — discriminate (only on a reproducing configuration)
- [ ] H1…H9, see §2.

### Phase 4 — workarounds and cost
- [ ] M0…M6 on `base` (+ `nohosted` as speed reference), see §5.

### Phase 5 — conclude
- [ ] §12 in `idf6_sd_fuckup.md`, recommendation for graceloader & co, upstream report draft if a driver bug is confirmed.

## 2. Hypotheses

| ID | Hypothesis | Test | Status | Evidence |
|---|---|---|---|---|
| H1 | `dma_aligned_buffer` interaction | `REMOUNT dmabuf=0` | refuted once on graceloader (fuckup §7); retest here | |
| H2 | DMA into PSRAM itself (mapping / 200 M timing) | psram-aligned vs psram-unaligned vs internal; 0xEE runs; address correlation | open | |
| H3 | SDMMC controller sharing with ESP-Hosted | variant `nohosted` | open | |
| H4 | cache msync (non-chunked C2M) | variant `chunkedmsync` | open | |
| H5 | DFS / PM clock changes | variant `nopm` | open | |
| H6 | PSRAM 200 MHz as DMA target | variant `psram80m` | open | |
| H7 | descriptor ring wrap (fuckup §8) | variant `ringfix` | open | |
| H8 | ISR refill / slot interleave | variant `isrtrace` (instrumented driver) | open | |
| H9 | other 6.0.2 driver change | variant `drv551` / bisect | open | |

## 3. Run log

| Date | Commit | Variant | Suite / args | Result file | Verdict | Notes |
|---|---|---|---|---|---|---|
| 09-14 | phase1 | base | smoke selftest=1 | `results/20260914T175704Z-base-smoke` | bad (planted) | detector self-test, as intended |
| 09-14 | phase1 | base | smoke | `results/20260914T175826Z-base-smoke` | ok | 4K/64K/256K direct into PSRAM fine; ~12.5 MB/s |
| 09-14 | 77adbe0 | base | matrix | `results/20260914T175924Z-base-matrix` | ok | 258/258, all dests, 4 K … 4 M |
| 09-14 | 77adbe0+ | base | grace file=paperclips app.so | `results/*-base-grace` | ok | 38/38, ELF seg 179804 B @4096 → 0x482b5000 |
| 09-14 | 77adbe0+ | base | sweep | `results/*-base-sweep` | ok | buffers 0x482b… → 0x499…, 36/36 |
| 09-14 | 77adbe0+ | base | grace usbhost=1 file=… | `results/*-base-grace` | ok | USB HID host running: still fine |
| 09-14 | 77adbe0+ | graceenv | grace file=… | `results/20260914T181244Z-graceenv-grace` | **BAD + crash** | 3rd case (150000 @4096 → 0x48082000): 130 bad bytes in 33 scattered 4-byte words; then panic in `heap_caps_free` (TLSF block header below the buffer corrupt) |
| 09-14 | 77adbe0+ | graceenv | canary idle=10 reads=20 | `results/20260914T181630Z-graceenv-canary` | **BAD + crash** | idle 10 s: canary clean. 1st read (→ 0x48292000): 158 bad bytes, words `0xffffffff` / untouched fill; 2 MB canary below it clean; next `aligned_alloc` panicked: PSRAM heap lock pointer = 0xffffffff (heap control struct at PSRAM start corrupt). Panic was HALT → badge hung, user power-cycled. |

## 4. Tried / dead ends

- (from graceloader, 2026-09-06) `host.dma_aligned_buffer = NULL` did not fix
  graceloader's ELF load (fuckup §7).

## 5. Workaround scoreboard

| Method | 150 K | 1 M | 4 M | Correct? |
|---|---|---|---|---|
| M0 direct fread into aligned PSRAM | | | | |
| M1 staged 4 K internal | | | | |
| M2 staged 16/32/64 K internal | | | | |
| M3 direct, chunks ≤ 16 K / ≤ 12 K | | | | |
| M4 unaligned PSRAM (driver bounce), dmabuf on/off | | | | |
| M5 setvbuf internal 8 K / 64 K | | | | |
| M6 driver-level fix | | | | |

## 6. Notes

### 0.3 sdkconfig: graceloader vs `base` (2026-09-14)

Only 44 diff lines. Relevant for SD/PSRAM/cache/PM/WDT: **none** — SPIRAM,
cache, PM/DFS, INT WDT (300 ms), assertions, RTC-fast heap, SDMMC and
ESP-Hosted SDIO settings are identical. Differences:

- graceloader has its own `FATFS_USE_FASTOPEN=y`, `FATFS_MAX_FILES_OPEN=8`,
  `FATFS_STDIO_BUF_SIZE=8192` (Kconfig from its kbelf component, not IDF;
  added *with* the workaround). Reproduced in the `grace` suite via `setvbuf`.
- `base` has ESP-Hosted NimBLE HCI over VHCI enabled (`bt` component is
  required by main); graceloader has it off. Disabled in `sdkconfigs/tanmatsu`
  to match.
- graceloader has USB host options (it links usb host); irrelevant.

### graceenv reproduction (2026-09-14)

- Only the graceloader-like boot reproduces; `base` (BSP initialised, display
  framebuffer occupying low PSRAM) never failed, not even with the USB host
  started at runtime.
- Corruption is **scattered single 4-byte words** (not 4 KB / 512 B blocks):
  words missing (still the fill value) or `0xffffffff`. Word granularity points
  away from descriptor-chain/sector bugs.
- At the same time memory **outside** the buffer is damaged: the TLSF header
  just below the buffer, and the PSRAM heap's control structure near
  0x48000000. A 2 MB canary block between those was **not** touched, and no
  stray writes happened while idle. So the damage hits memory the CPU recently
  wrote through the cache (heap metadata, freshly memset buffer) during SD DMA
  into PSRAM — suggests a cache/PSRAM-access interaction rather than DMA to
  random addresses.
- Addresses: failing buffers at 0x48082000 / 0x48292000 (low PSRAM, free
  because no framebuffer); base's buffers start at 0x482b…

### Recovery (after the hang)

`CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y` in `sdkconfigs/tanmatsu` (was HALT).
`tools/recover.py` / `make recover`: hard reset over `$PORT` as `make monitor`
does → app restarts → READY → `EXIT` → launcher. `tools/sdtest.py` runs it
automatically after crash / stall / lost link. Verified 2026-09-14.

## How to run

Everything is driven from the host; the badge only needs to sit in the
launcher (or in BadgeLink mode).

```sh
make cycle VARIANT=base SUITE=matrix             # build, install, run, test, exit to launcher
make cycle VARIANT=base SUITE=stress ARGS="dest=psram size=150000 off=4096 reps=30"
make testrun VARIANT=base SUITE=grace ARGS="file=/sd/apps/at.cavac.paperclips-grace/app.so"   # app already running
make cycleall SUITE=matrix                       # all VARIANTS
```

- `tools/sdtest.py` connects to `$PORT`, waits for READY/PONG, sends
  `RUN <suite> <args>`, stores `results/<UTC>-<variant>-<suite>.{log,json}`,
  sends `EXIT`. Exit code 0 ok, 3 corrupted reads, 2 crash/error, 1 link/timeout.
- Suites and args: see the header comment of `main/sdtest.c`. Common args:
  `dmabuf=0|1`, `io=unbuf|default|fast`, `vbuf=`, `reps=`, `display=1`
  (redraw per line), `wifi=1` (hosted variants), `selftest=1`.
- Variants: `sdkconfigs/variant-<name>` (+ `patches/<name>/<idf component>/*.patch`
  for patched IDF components).
- The app returns to the launcher by itself after 5 min without commands.
- There is **one** app, `at.cavac.idf6test`, installed straight into AppFS
  (no `/int` or `/sd` app directory). Each `make install VARIANT=…` overwrites
  it; `sdtest.py --variant` refuses to test a mismatching build.
