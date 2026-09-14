# Draft: ESP32-P4 SDMMC DMA into PSRAM corrupts memory when DFS is enabled

*Draft for espressif/esp-idf. Not filed yet.*

## Environment

- ESP-IDF v6.0.2 (`7101770dc6db2667b3c477cc31365dd1acd6db4e`)
- ESP32-P4 rev v1.3, 32 MB hex PSRAM at 200 MHz (`CONFIG_SPIRAM_SPEED_200M=y`)
- `CONFIG_PM_ENABLE=y`, `CONFIG_PM_DFS_INIT_AUTO=y` (360 ↔ 40 MHz)
- SD card on SDMMC slot 0, 4-bit, 40 MHz, FATFS via `esp_vfs_fat_sdmmc_mount()`
- No MIPI DSI panel initialised

## Symptom

When reading a file from the SD card with `fread()` into a PSRAM buffer, the
data is sometimes wrong. The first ~150 KB read typically fails:

- single 4-byte words in the destination stay untouched or read `0xffffffff`;
- PSRAM outside the destination is damaged too, mostly memory the CPU wrote
  just before, such as TLSF block headers or the PSRAM heap's control
  structure;
- the next `malloc()` or `free()` then hits a load or store access fault.

Occasionally the system locks up instead: an interrupt watchdog fires in
`mspi_timing_change_speed_mode_cache_safe()`, or the chip gets a
`HP_SYS_HP_WDT_RESET`.

## Cause

- `components/esp_driver_sdmmc/src/sd_host_sdmmc.c` creates the controller PM
  lock as `ESP_PM_APB_FREQ_MAX`. On the P4 this does not keep the CPU at or
  above `CONFIG_SPIRAM_SPEED`.
- While a transfer runs, DFS can still switch the CPU below 200 MHz.
  `esp_clk_utils_mspi_speed_mode_sync_before/after_cpu_freq_switching()` then
  moves the PSRAM MSPI interface between the 20 MHz low-speed mode and the
  high-speed mode.
- The DMA writes into PSRAM while this happens. The AXI-vs-PSRAM frequency
  constraint noted in `clk_utils.c` is violated.

The DSI DPI driver already guards against exactly this
(`esp_lcd_panel_dpi.c`, `ESP_PM_CPU_FREQ_MAX`, "to ensure PSRAM bandwidth and
usability during DFS"). The SDMMC host driver does not.

## Reproduction

Minimal app: `CONFIG_PM_ENABLE=y`, DFS auto, mount the SD card, then in a loop:

1. `aligned_alloc(4096, 150000)` in PSRAM;
2. `fread()` 150 KB from a known file;
3. compare the result, then free the buffer.

This fails or crashes within a few iterations. Holding an
`ESP_PM_CPU_FREQ_MAX` lock around the loop makes it pass: 0 errors in
several hundred reads from 4 K to 4 M.

The full automated test app with results is at
<https://github.com/…/tanmatsu-idf6tests> (fill in before filing).

## Suggested fix

On targets with `MSPI_TIMING_LL_PSRAM_FREQ_AXI_CONSTRAINED` and
`CONFIG_SPIRAM`, create the SDMMC PM lock as `ESP_PM_CPU_FREQ_MAX`, as the DSI
driver does:

```diff
-    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "sdmmc", &ctlr->pm_lock), err, TAG, "failed to create pm lock");
+    ESP_GOTO_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sdmmc", &ctlr->pm_lock), err, TAG, "failed to create pm lock");
```

Verified on hardware: 30/30 canary reads, 258/258 read matrix and 95/95 ELF
segment loads, where the unpatched build fails every run.

Other DMA-capable drivers that only take `ESP_PM_APB_FREQ_MAX` and may target
PSRAM probably need the same check.

## Open question

The same application was reported working on IDF v5.5.1, and the DFS/MSPI
code paths look equivalent there. We have not built the reproduction on
v5.5.1 yet.
