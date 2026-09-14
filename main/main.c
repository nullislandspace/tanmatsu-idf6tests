// IDF6 SD card test -- see TESTPLAN.md and idf6_sd_fuckup.md.
//
// Mounts the SD card the way tanmatsu-graceloader does, then waits for
// commands on the debug console (see console.h), normally sent by
// tools/sdtest.py. Each RUN executes one sdtest suite; results go out as
// CRC-framed records, as text on the console and screen, and are appended to
// /sd/idf6test/results-<variant>.txt.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "console.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gl_input.h"
#include "hal/usb_serial_jtag_ll.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "report.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "sdtest.h"

#if CONFIG_IDF6TEST_WITH_HOSTED
#include "wifi_connection.h"
#include "wifi_remote.h"
#endif

static char const TAG[] = "idf6test";

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF

#define FONT_SIZE         9
#define SCREEN_LINES      52
#define LINE_LEN          160
#define REPORT_BYTES      (512 * 1024)
#define REDRAW_PERIOD_US  (250 * 1000)

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

static char    screen_lines[SCREEN_LINES][LINE_LEN];
static int     screen_count = 0;
static char*   report_buf   = NULL;
static size_t  report_len   = 0;
static bool    redraw_every_line = false;  // RUN ... display=1
static int64_t last_redraw  = 0;

// ---- Output -----------------------------------------------------------------

static void redraw(void) {
    if (pax_buf_get_width(&fb) == 0) return;
    pax_background(&fb, BLACK);
    int first = screen_count > SCREEN_LINES ? screen_count - SCREEN_LINES : 0;
    for (int i = first, y = 0; i < screen_count; i++, y++) {
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, FONT_SIZE, 2, y * FONT_SIZE, screen_lines[i % SCREEN_LINES]);
    }
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
    last_redraw = esp_timer_get_time();
}

// A full-screen blit per line would slow runs down and add display DMA
// activity in between reads, so redraws are throttled unless display=1.
static void log_line(const char* line) {
    printf("%s\n", line);
    snprintf(screen_lines[screen_count % SCREEN_LINES], LINE_LEN, "%s", line);
    screen_count++;
    if (report_buf) {
        int n = snprintf(report_buf + report_len, REPORT_BYTES - report_len, "%s\n", line);
        if (n > 0 && report_len + n < REPORT_BYTES) report_len += n;
    }
    if (redraw_every_line || esp_timer_get_time() - last_redraw > REDRAW_PERIOD_US) redraw();
}

static void save_report(void) {
    if (!report_buf || !sdcard_is_mounted()) return;
    char path[64];
    snprintf(path, sizeof(path), "/sd/idf6test/results-%s.txt", CONFIG_IDF6TEST_VARIANT_NAME);
    FILE* f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", path);
        return;
    }
    fwrite(report_buf, 1, report_len, f);
    fprintf(f, "\n");
    fclose(f);
}

// ---- Display setup (from tanmatsu-template) ----------------------------------

static void init_display(void) {
    esp_err_t res =
        bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "no display parameters: %d", res);
        return;
    }
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, PAX_BUF_24_888RGB);
    pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90:
            pax_buf_set_orientation(&fb, PAX_O_ROT_CCW);
            break;
        case BSP_DISPLAY_ROTATION_180:
            pax_buf_set_orientation(&fb, PAX_O_ROT_HALF);
            break;
        case BSP_DISPLAY_ROTATION_270:
            pax_buf_set_orientation(&fb, PAX_O_ROT_CW);
            break;
        default:
            break;
    }
}

// ---- Commands ---------------------------------------------------------------

#if CONFIG_IDF6TEST_WITH_HOSTED
// Runs on the main task, whose stack is in internal RAM: wifi_connect_try_all()
// reads the saved networks from NVS, which needs that.
static bool connect_wifi(void) {
    log_line("Connecting to WiFi (networks saved in the launcher)...");
    if (wifi_remote_initialize() != ESP_OK) {
        log_line("  radio not responding");
        return false;
    }
    wifi_connection_init_stack();
    if (wifi_connect_try_all() != ESP_OK) {
        log_line("  could not connect");
        return false;
    }
    log_line("  connected");
    return true;
}
#endif

static bool usb_host_started = false;

static void start_usb_host(void) {
    if (usb_host_started) return;
    log_line("Starting USB HID host (gl_input_init, as graceloader)");
    gl_input_init();
    usb_host_started = true;
}

// tanmatsu-graceloader main.c at 7035d75, steps before mounting the SD card.
// Done at boot in the graceenv variant; RUN args usbphy=1 / intmount=1 do them
// later in any variant.
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
static bool        usb_phy_switched = false;

static void switch_usb_phy(void) {
    if (usb_phy_switched) return;
    usb_phy_switched = true;
    const usb_serial_jtag_pull_override_vals_t override_disable_usb = {
        .dm_pd = true, .dm_pu = false, .dp_pd = true, .dp_pu = false};
    const usb_serial_jtag_pull_override_vals_t override_enable_usb = {
        .dm_pd = false, .dm_pu = false, .dp_pd = false, .dp_pu = true};
    usb_serial_jtag_ll_phy_enable_pull_override(&override_disable_usb);
    usb_serial_jtag_ll_phy_select(0);
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_serial_jtag_ll_phy_enable_pull_override(&override_enable_usb);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

static void mount_int(void) {
    if (wl_handle != WL_INVALID_HANDLE) return;
    esp_vfs_fat_mount_config_t fat_config = {
        .format_if_mount_failed   = false,
        .max_files                = 10,
        .allocation_unit_size     = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };
    esp_err_t res = esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &fat_config, &wl_handle);
    if (res != ESP_OK) ESP_LOGE(TAG, "Failed to mount /int: %s", esp_err_to_name(res));
}

// RUN ... pmlock=cpu|apb|nosleep holds that PM lock for the duration of the
// suite. The DSI display driver holds ESP_PM_CPU_FREQ_MAX while the display
// runs ("to ensure PSRAM bandwidth and usability during DFS"); the SDMMC
// driver only takes ESP_PM_APB_FREQ_MAX per transaction.
static esp_pm_lock_handle_t pm_locks[3];

static esp_pm_lock_handle_t get_pm_lock(const char* args) {
    char name[16];
    if (!sdtest_arg_str(args, "pmlock", name, sizeof(name))) return NULL;
    static const char* const names[3] = {"cpu", "apb", "nosleep"};
    static const esp_pm_lock_type_t types[3] = {ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP};
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, names[i]) != 0) continue;
        if (!pm_locks[i] && esp_pm_lock_create(types[i], 0, "idf6test", &pm_locks[i]) != ESP_OK) return NULL;
        return pm_locks[i];
    }
    return NULL;
}

static void emit_begin(const char* suite, const char* args) {
    sdmmc_card_t* card = sdcard_get_card();
    report_emitf("BEGIN",
                 "{\"t\":\"begin\",\"variant\":\"%s\",\"suite\":\"%s\",\"args\":\"%s\",\"idf\":\"%s\","
                 "\"hosted\":%d,\"chunked_msync\":%d,\"pm\":%d,\"psram_mhz\":%d,\"int_wdt_ms\":%d,"
                 "\"bsp_init\":%d,\"grace_boot\":%d,\"usb_host\":%d,\"int_mounted\":%d,"
                 "\"reset_reason\":%d,\"free_int\":%u,\"largest_int\":%u,\"free_psram\":%u,"
                 "\"card_name\":\"%s\",\"card_mb\":%llu,\"card_khz\":%d}",
                 CONFIG_IDF6TEST_VARIANT_NAME, suite, args, esp_get_idf_version(),
#if CONFIG_IDF6TEST_WITH_HOSTED
                 1,
#else
                 0,
#endif
#if CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS
                 1,
#else
                 0,
#endif
#if CONFIG_PM_ENABLE
                 1,
#else
                 0,
#endif
                 CONFIG_SPIRAM_SPEED, CONFIG_ESP_INT_WDT_TIMEOUT_MS,
#if CONFIG_IDF6TEST_BSP_INIT
                 1,
#else
                 0,
#endif
#if CONFIG_IDF6TEST_GRACE_BOOT
                 1,
#else
                 0,
#endif
                 usb_host_started, wl_handle != WL_INVALID_HANDLE, (int)esp_reset_reason(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), card ? card->cid.name : "",
                 card ? (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024) : 0ULL,
                 card ? card->real_freq_khz : 0);
}

static void run_command(char* line) {
    // "RUN <suite> [args]"
    char* suite = line + 4;
    while (*suite == ' ') suite++;
    char* args = strchr(suite, ' ');
    if (args) {
        *args++ = '\0';
    } else {
        args = "";
    }

    report_len        = 0;
    redraw_every_line = sdtest_arg_int(args, "display", 0) != 0;
    char text[LINE_LEN];
    log_line("==============================================================");
    snprintf(text, sizeof(text), "IDF6 SD test, variant %s: RUN %s %s", CONFIG_IDF6TEST_VARIANT_NAME, suite, args);
    log_line(text);
    if (sdtest_arg_int(args, "usbphy", 0)) switch_usb_phy();
    if (sdtest_arg_int(args, "intmount", 0)) mount_int();
    if (sdtest_arg_int(args, "usbhost", 0)) start_usb_host();
    emit_begin(suite, args);

    esp_pm_lock_handle_t pm_lock = get_pm_lock(args);
    if (pm_lock) esp_pm_lock_acquire(pm_lock);
    int64_t     t0     = esp_timer_get_time();
    const char* status = "error";
    if (!sdcard_is_mounted()) {
        log_line("SD card not mounted");
    } else {
#if CONFIG_IDF6TEST_WITH_HOSTED
        if (sdtest_arg_int(args, "wifi", 0) && !connect_wifi()) {
            suite = "wifi-failed";  // not a suite: sdtest_run reports an error
        }
#endif
        switch (sdtest_run(suite, args, log_line)) {
            case SDTEST_OK:
                status = "ok";
                break;
            case SDTEST_BAD:
                status = "bad";
                break;
            default:
                status = "error";
                break;
        }
    }
    if (pm_lock) esp_pm_lock_release(pm_lock);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    snprintf(text, sizeof(text), "Run took %lld ms", ms);
    log_line(text);
    redraw();
    save_report();
    report_emitf("END", "{\"t\":\"end\",\"suite\":\"%s\",\"status\":\"%s\",\"ms\":%lld}", suite, status, ms);
}

static void return_to_launcher(void) {
    report_emitf("BYE", "{\"t\":\"bye\"}");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));  // let the USB FIFO drain
    bsp_device_restart_to_launcher();
}

// ---- Main -------------------------------------------------------------------

void app_main(void) {
    gpio_install_isr_service(0);

    // Not erased on failure: NVS holds the launcher's WiFi networks.
    esp_err_t res = nvs_flash_init();
    if (res != ESP_OK) ESP_LOGE(TAG, "NVS init failed: %d", res);

#if CONFIG_IDF6TEST_BSP_INIT
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }
    init_display();
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
#endif

    report_buf = heap_caps_malloc(REPORT_BYTES, MALLOC_CAP_SPIRAM);

#if CONFIG_IDF6TEST_GRACE_BOOT
    switch_usb_phy();
    mount_int();
#endif

    // As graceloader: SD card straight after the BSP, radio left as it is.
    char text[LINE_LEN];
    res = sdcard_init();
    if (res != ESP_OK) {
        snprintf(text, sizeof(text), "SD card mount failed: %s", esp_err_to_name(res));
    } else {
        snprintf(text, sizeof(text), "IDF6 SD test (%s) ready. Waiting for tools/sdtest.py. F1/Esc: launcher",
                 CONFIG_IDF6TEST_VARIANT_NAME);
    }
    log_line(text);
    redraw();

#if CONFIG_IDF6TEST_GRACE_BOOT
    start_usb_host();
#endif

    QueueHandle_t commands = console_start();
    for (;;) {
        console_cmd_t cmd;
        if (xQueueReceive(commands, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (strncmp(cmd.line, "RUN ", 4) == 0) {
                console_set_busy(true);
                run_command(cmd.line);
                console_set_busy(false);
            } else if (strcmp(cmd.line, "EXIT") == 0) {
                return_to_launcher();
            }
        }
        bsp_input_event_t event;
        while (input_event_queue && xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state &&
                (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1 ||
                 event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC)) {
                return_to_launcher();
            }
        }
    }
}
