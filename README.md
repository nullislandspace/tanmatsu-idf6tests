# tanmatsu-idf6tests

Test app that tracked down SD card read corruption on the Tanmatsu (ESP32-P4)
under ESP-IDF 6.0.2, first seen in tanmatsu-graceloader.

**Result:** with `CONFIG_PM_ENABLE=y` and no display running, dynamic frequency
scaling switches the PSRAM MSPI interface to low-speed mode during SD DMA
transfers into PSRAM (the SDMMC driver only holds `ESP_PM_APB_FREQ_MAX`). Words
go missing and PSRAM heap metadata is damaged; the system can also lock up.
Holding `ESP_PM_CPU_FREQ_MAX` fixes it without a speed penalty.

- `idf6_sd_fuckup.md` — the full investigation; §12 has the results.
- `TESTPLAN.md` — plan, hypotheses, run log, workaround speeds, how to run.
- `upstream-report.md` — draft issue for espressif/esp-idf (not filed).
- `results/` — raw console logs and parsed JSON of every device run.

## Running

The badge sits in the launcher; everything is driven over the debug console
(`PORT`) and BadgeLink (`BADGELINKPORT`):

`BADGELINKPORT` is empty by default, which makes the tools talk to BadgeLink
over raw USB (`16d0:0f9a`); set it to `host:port` to go through a TCP proxy
such as badgenet's `localhost:4003`.

```sh
make cycle VARIANT=nobsp SUITE=canary ARGS="reads=30"            # build, install, run, capture
make cycle VARIANT=nobsp SUITE=canary ARGS="reads=30 pmlock=cpu" # same with the fix
make recover                                                    # after a hang: reset + exit to launcher
```

One app (`at.cavac.idf6test`) is installed straight into AppFS and overwritten
by each variant. See `TESTPLAN.md` for suites, arguments and variants.

## License

The contents of this repository may be considered in the public domain or
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at your
disposal.
