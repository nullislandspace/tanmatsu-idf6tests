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
- [x] 1.1 Firmware command console (USB-serial/JTAG driver, PING/RUN/EXIT, READY banner, 5 min idle timeout → launcher). `REMOUNT` was planned but not needed (see 1.4).
- [x] 1.2 Framed, CRC-checked result records.
- [x] 1.3 `main.c` restructure (no startup delay, throttled redraw).
- [x] 1.4 Suites: smoke, matrix, grace (+ real ELF via `file=`), stress, workaround; `selftest=1`; `wifi=1` connects WiFi before any suite. No `REMOUNT` needed: `dmabuf=0/1` switches `card->host.dma_aligned_buffer` live.
- [x] 1.5 Host scripts: `tools/sdtest.py`, `tools/badgelink_retry.sh`.
- [x] 1.6 Makefile: verified `mode_badgelink`, AppFS install, `testrun`, `cycle`, `cycleall`, generic variant patches.
- [x] 1.7 Two back-to-back unattended cycles OK. Self-test: planted block reported as `+4096 len 4096: data from file offset 12288 (+8192)`, 4096-byte granularity. Pattern switched to an invertible hash per word so byte counts/runs are exact.

### Phase 2 — reproduce on `base`
- [x] 2.1 `base`: `matrix` 258/258 ok, `grace` (+ real `app.so` segments) 38/38 ok, `sweep` over 25 MB of PSRAM addresses 36/36 ok, `grace usbhost=1` ok. **Not reproduced on base.**
- [x] 2.3 Approached graceloader's boot: new variant `graceenv` (no `bsp_device_initialize()`, USB PHY switch, `/int` WL mount, USB HID host — exactly graceloader's `app_main` before the load). **Reproduced** on the first run (see §6 "graceenv reproduction").
- [x] 2.2 Bisected: `nobsp` (only difference to base: no `bsp_device_initialize()`) reproduces without any of graceloader's other boot steps. The BSP's DSI display driver holds `ESP_PM_CPU_FREQ_MAX`; without it DFS runs.

### Phase 3 — discriminate (only on a reproducing configuration)
- [x] Root cause found: **H12** (DFS/MSPI low-speed switching during SD DMA into PSRAM), see §2 and §6. H1–H9 not needed any more.

### Phase 4 — workarounds and cost
- [x] Measured on `pmfix` (all methods correct there), see §5. App-level lock validated in `graceenv`.

### Phase 5 — conclude
- [x] §12 in `idf6_sd_fuckup.md`; `upstream-report.md` draft. Open: why IDF 5.5.1 did not show it (DFS/MSPI code paths are equivalent).

## 2. Hypotheses

| ID | Hypothesis | Test | Status | Evidence |
|---|---|---|---|---|
| H1 | `dma_aligned_buffer` interaction | `dmabuf=0` | not the cause (graceloader §7; fix works with it set). Costs speed on unaligned reads, see §5 | `pmfix-workaround` |
| H2 | DMA into PSRAM itself (address mapping) | `sweep` over 25 MB of PSRAM | refuted (addresses irrelevant on base) | `20260914T180356Z-base-sweep` |
| H3 | SDMMC controller sharing with ESP-Hosted | variant `nohosted` | not needed: fix works with hosted on | `pmfix`, `gracefix` |
| H4 | cache msync (non-chunked C2M) | variant `chunkedmsync` | not needed | |
| H5 | DFS / PM clock changes | runtime PM locks | → H12 | |
| H6 | PSRAM 200 MHz as DMA target | variant `psram80m` | → H12 (the MSPI speed *switch* is the problem) | |
| H7 | descriptor ring wrap (fuckup §8) | variant `ringfix` | not needed: unpatched ring + CPU lock is correct | `pmfix-matrix` |
| H8 | ISR refill / slot interleave | variant `isrtrace` (instrumented driver) | not needed | |
| H9 | other 6.0.2 driver change | variant `drv551` / bisect | not needed | |
| H10 | USB host (graceloader's gl_input) | `base usbhost=1` | refuted | `20260914T180959Z-base-grace` |
| H11 | graceloader boot environment | variants `graceenv`, `nobsp` | **reproduces**; `nobsp` alone is enough | `181244Z-graceenv-grace`, `182051Z-nobsp-canary` |
| H12 | **DFS: SDMMC takes only `ESP_PM_APB_FREQ_MAX`, CPU/AXI drops below PSRAM speed, MSPI switches to low-speed mode during DMA into PSRAM** | `pmlock=cpu/apb/nosleep`, variants `pmfix`, `gracefix` | **confirmed** | see §6 |

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
| 09-14 | 7ee84cb | nobsp | canary idle=3 reads=10 | `20260914T182051Z-nobsp-canary` | **BAD + crash** | no BSP init, no other graceloader steps: reproduces |
| 09-14 | 7ee84cb+ | nobsp | canary reads=20 **pmlock=cpu** | `20260914T183555Z-nobsp-canary` | ok | 20/20 |
| 09-14 | 7ee84cb+ | nobsp | canary reads=20 (no lock) | `20260914T183618Z-nobsp-canary` | **BAD + crash** | control; auto-recovery worked |
| 09-14 | 7ee84cb+ | nobsp | canary pmlock=apb | `20260914T183653Z-nobsp-canary` | crash | interrupt WDT on CPU1 in `mspi_timing_change_speed_mode_cache_safe` during baseline |
| 09-14 | 7ee84cb+ | nobsp | canary pmlock=nosleep | `20260914T183728Z-nobsp-canary` | crash | same interrupt WDT |
| 09-14 | 7ee84cb+ | nobsp | matrix pmlock=cpu | `20260914T183919Z-nobsp-matrix` | ok | 258/258 |
| 09-14 | 7ee84cb+ | nobsp | grace reps=5 pmlock=cpu file=… | `20260914T184005Z-nobsp-grace` | ok | 95/95 |
| 09-14 | 8aaa7bc | pmfix | canary reads=30 | `20260914T191812Z-pmfix-canary` | ok | driver patch only, no app lock |
| 09-14 | 8aaa7bc | pmfix | matrix | `20260914T191834Z-pmfix-matrix` | ok | 258/258 |
| 09-14 | 8aaa7bc | pmfix | grace reps=5 file=… | `20260914T191920Z-pmfix-grace` | ok | 95/95 |
| 09-14 | 8aaa7bc | nobsp | canary reads=30 | `20260914T192200Z-nobsp-canary` | **BAD + crash** | drift check after pmfix: still fails |
| 09-14 | 8aaa7bc+ | gracefix | canary reads=30 | `20260914T192422Z-gracefix-canary` | ok | graceloader env + driver patch |
| 09-14 | 8aaa7bc+ | gracefix | grace reps=5 file=… | `20260914T192446Z-gracefix-grace` | ok | 95/95 |
| 09-14 | 8aaa7bc+ | nobsp | canary method=staged chunk=4096 | `20260914T192556Z-nobsp-canary` | ok | graceloader's current workaround, 150 K: 30/30 |
| 09-14 | 8aaa7bc+ | nobsp | canary method=staged size=1048576 | `20260914T192615Z-nobsp-canary` | **HW WDT reset** | `rst:0x7 HP_SYS_HP_WDT_RESET` during the 4 K internal baseline reads — staging does not prevent lock-ups |
| 09-14 | 8aaa7bc+ | pmfix | workaround reps=3 | `20260914T192853Z-pmfix-workaround` | ok | 108/108, speeds in §5 |
| 09-14 | 8aaa7bc+ | graceenv | canary reads=30 pmlock=cpu | `20260914T193205Z-graceenv-canary` | ok | app-level lock in graceloader env, unpatched driver |
| 09-14 | 8aaa7bc+ | graceenv | grace reps=5 pmlock=cpu file=… | `20260914T193229Z-graceenv-grace` | ok | 95/95 |

## 4. Tried / dead ends

- (from graceloader, 2026-09-06) `host.dma_aligned_buffer = NULL` did not fix
  graceloader's ELF load (fuckup §7).
- `base` never reproduces, whatever the read pattern, address (sweep) or USB
  host: the display driver's PM lock hides the bug.
- PM locks `apb` and `nosleep` are not enough (interrupt WDT in the MSPI speed
  switch); only `ESP_PM_CPU_FREQ_MAX` works.
- Graceloader's 4 K staging avoids the corrupted *bytes* but not the
  DFS-related lock-ups (HW WDT reset seen during plain internal reads).
- Invalid run removed: `20260914T190309Z-pmfix-canary` captured the launcher
  booting (pmfix install had failed on a full `/int`), not the test app.
- Installing one app per variant into `/int` filled it; now one AppFS-only app.

## 5. Workaround scoreboard

| Method | 150 K | 1 M | 4 M | Correct? |
|---|---|---|---|---|
Measured on `pmfix` (driver fixed, so every method is correct; KB/s, mean of 3), `20260914T192853Z-pmfix-workaround`:

| Method | 150 K | 1 M | 4 M | Correct without the PM fix? |
|---|---|---|---|---|
| M0 direct fread into aligned PSRAM | 12022 | 13006 | 13215 | **no** |
| M0' default stdio / fast 8 K / fast 64 K | 11992 / 12034 / 12023 | 13004 / 13010 / 13008 | 13219 / 13221 / 13220 | **no** |
| M1 staged 4 K internal (graceloader today) | 6781 | 6769 | 6849 | bytes yes, lock-ups possible |
| M2 staged 16 / 32 / 64 K internal | 9103 / 9912 / 10325 | 9305 / 10248 / 10851 | 9434 / 10390 / 10998 | (not tested) |
| M3 direct, chunks 12 K / 16 K | 9686 / 9823 | 10349 / 10398 | 10577 / 10583 | (not tested) |
| M4 unaligned PSRAM, dmabuf=2 KB (graceloader) / NULL | 4303 / 9178 | 4330 / 9335 | 4456 / 9457 | (not tested) |
| **M6 hold `ESP_PM_CPU_FREQ_MAX` (app) or pmfix (driver)** | = M0 | = M0 | = M0 | **yes** (canary/matrix/grace) |

Side finding: graceloader's 2 KB `host.dma_aligned_buffer` halves unaligned-read
speed (4.3 vs 9.3 MB/s) because every bounce is 4 sectors.

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

### Root cause (2026-09-14)

- `CONFIG_PM_ENABLE=y` + `CONFIG_PM_DFS_INIT_AUTO=y`: CPU scales between 360
  and 40 MHz.
- On the P4, whenever the target CPU frequency is below `CONFIG_SPIRAM_SPEED`
  (200), `esp_clk_utils_mspi_speed_mode_sync_*` calls
  `mspi_timing_change_speed_mode_cache_safe()`, which stalls the other core,
  freezes the ext-mem cache and switches flash/PSRAM MSPI to a 20 MHz
  low-speed mode (and back). IDF comment: AXI frequency must stay ≥ PSRAM
  frequency "to avoid writing MSPI FIFO overflow".
- The SDMMC driver (`sd_host_sdmmc.c:105`) only creates an
  `ESP_PM_APB_FREQ_MAX` lock. On the P4 that does not keep the CPU at or above
  200 MHz, so DFS switches happen during SD DMA transfers into PSRAM →
  scattered lost/`0xffffffff` words in the destination *and* damaged PSRAM
  memory the CPU wrote shortly before (heap metadata) → crashes. It can also
  deadlock (interrupt WDT / HP WDT reset) inside the speed switch.
- The MIPI DSI DPI driver holds `ESP_PM_CPU_FREQ_MAX` for its lifetime
  ("to ensure PSRAM bandwidth and usability during DFS"). Every app that
  initialises the BSP display is therefore immune; graceloader loads apps
  **before** anything initialises the display.
- Fix: hold `ESP_PM_CPU_FREQ_MAX` while using the SD card (app level), or patch
  the driver (`patches/pmfix/esp_driver_sdmmc/cpu-freq-max-pm-lock.patch`).
  Both verified; no throughput cost.
- Open: why IDF 5.5.1 was not affected. `clk_utils.c`, `pm_impl.c` APB/CPU
  mode selection and `mspi_timing_enter_low_speed_mode` (20 MHz) are
  equivalent in both trees. Would need an IDF 5.5.1 build of `nobsp`.

### graceenv reproduction (2026-09-14)

*(First observations, written before the cause was known. The "cache
interaction" reading below is superseded by "Root cause" above: the damaged
memory is PSRAM accessed while MSPI switches speed mode.)*

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

### Unexplained, minor

- `base` `matrix` measured ~7.8 MB/s for large direct reads, while `smoke`,
  `sweep` and `pmfix` `workaround` measured 12–13 MB/s for the same kind of
  read. Not investigated (possibly the many small reads/log lines in between).
- The first `PING` after an app start sometimes arrives prefixed with console
  junk (`Unknown command: I (PING`); the host retries every second, harmless.

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
- Suites (`smoke`, `matrix`, `stress`, `grace`, `workaround`, `canary`,
  `sweep`) and their args: header comment of `main/sdtest.c`. Args handled by
  `main.c` for any suite: `pmlock=cpu|apb|nosleep` (hold that PM lock during
  the suite), `usbphy=1`, `intmount=1`, `usbhost=1` (graceloader's boot
  steps), `display=1` (redraw per line), `wifi=1` (hosted variants).
- `make recover` resets the badge (as `make monitor` does) and sends `EXIT`;
  `sdtest.py` does this by itself after a crash, stall or lost link.
- Variants (`sdkconfigs/variant-<name>`, plus `patches/<name>/<idf component>/*.patch`):

  | Variant | What | Reproduces? |
  |---|---|---|
  | `base` | as graceloader's sdkconfig, BSP (display) initialised | no (DSI holds CPU_FREQ_MAX) |
  | `nobsp` | base without `bsp_device_initialize()` | **yes** |
  | `graceenv` | boots exactly like graceloader (no BSP, USB PHY switch, `/int`, USB HID host) | **yes** |
  | `pmfix` | nobsp + `esp_driver_sdmmc` taking `ESP_PM_CPU_FREQ_MAX` | no (fixed) |
  | `gracefix` | graceenv + the same patch | no (fixed) |
  | `nohosted`, `ringfix`, `chunkedmsync` | original hypotheses (ESP-Hosted off, ring wrap fix, chunked msync) | not run: made unnecessary by the root cause |
- The app returns to the launcher by itself after 5 min without commands.
- There is **one** app, `at.cavac.idf6test`, installed straight into AppFS
  (no `/int` or `/sd` app directory). Each `make install VARIANT=…` overwrites
  it; `sdtest.py --variant` refuses to test a mismatching build.
