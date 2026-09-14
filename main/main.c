// IDF6 SD card test -- see README.md and idf6_sd_fuckup.md.
//
// Mounts the SD card the way tanmatsu-graceloader does, then runs sdtest:
// large reads straight into PSRAM (and friends), each checked against a
// self-describing pattern file. The report goes to the console, the screen
// and /sd/idf6test/results-<variant>.txt.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
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

#define FONT_SIZE    9
#define SCREEN_LINES 52
#define LINE_LEN     160
#define REPORT_BYTES (256 * 1024)

static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

static char   screen_lines[SCREEN_LINES][LINE_LEN];
static int    screen_count = 0;
static char*  report_buf   = NULL;
static size_t report_len   = 0;

// ---- Output -----------------------------------------------------------------

static void redraw(void) {
    if (pax_buf_get_width(&fb) == 0) return;
    pax_background(&fb, BLACK);
    int first = screen_count > SCREEN_LINES ? screen_count - SCREEN_LINES : 0;
    for (int i = first, y = 0; i < screen_count; i++, y++) {
        pax_draw_text(&fb, WHITE, pax_font_sky_mono, FONT_SIZE, 2, y * FONT_SIZE, screen_lines[i % SCREEN_LINES]);
    }
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

static void log_line(const char* line) {
    printf("%s\n", line);
    snprintf(screen_lines[screen_count % SCREEN_LINES], LINE_LEN, "%s", line);
    screen_count++;
    if (report_buf) {
        int n = snprintf(report_buf + report_len, REPORT_BYTES - report_len, "%s\n", line);
        if (n > 0 && report_len + n < REPORT_BYTES) report_len += n;
    }
    redraw();
}

static void save_report(void) {
    if (!report_buf) return;
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
    printf("Report appended to %s\n", path);
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

// ---- Test run ---------------------------------------------------------------

static void header(const char* what) {
    char line[LINE_LEN];
    log_line("==============================================================");
    snprintf(line, sizeof(line), "IDF6 SD test, variant %s: %s", CONFIG_IDF6TEST_VARIANT_NAME, what);
    log_line(line);
    snprintf(line, sizeof(line), "ESP-IDF %s, ESP-Hosted %s, chunked C2M msync %s", esp_get_idf_version(),
#if CONFIG_IDF6TEST_WITH_HOSTED
             "on",
#else
             "off",
#endif
#if CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS
             "on"
#else
             "off"
#endif
    );
    log_line(line);
    snprintf(line, sizeof(line), "Free: internal %u (largest %u), PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    log_line(line);
}

static void run_tests(const char* what) {
    report_len = 0;
    header(what);
    int64_t t0 = esp_timer_get_time();
    sdtest_run(log_line);
    char line[LINE_LEN];
    snprintf(line, sizeof(line), "Run took %lld s", (esp_timer_get_time() - t0) / 1000000);
    log_line(line);
    save_report();
}

#if CONFIG_IDF6TEST_WITH_HOSTED
// Called from app_main, whose stack is in internal RAM: wifi_connect_try_all()
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

// ---- Main -------------------------------------------------------------------

void app_main(void) {
    gpio_install_isr_service(0);

    // Not erased on failure: NVS holds the launcher's WiFi networks.
    esp_err_t res = nvs_flash_init();
    if (res != ESP_OK) ESP_LOGE(TAG, "NVS init failed: %d", res);

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

    report_buf = heap_caps_malloc(REPORT_BYTES, MALLOC_CAP_SPIRAM);

    char line[LINE_LEN];
    snprintf(line, sizeof(line), "IDF6 SD test (%s): starting in %d s so the monitor can reconnect...",
             CONFIG_IDF6TEST_VARIANT_NAME, CONFIG_IDF6TEST_STARTUP_DELAY_MS / 1000);
    log_line(line);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_IDF6TEST_STARTUP_DELAY_MS));

    // As graceloader: SD card straight after the BSP, radio left as it is.
    res = sdcard_init();
    if (res != ESP_OK) {
        snprintf(line, sizeof(line), "SD card mount failed: %s. Press F1 to return to the launcher.",
                 esp_err_to_name(res));
        log_line(line);
    } else {
        run_tests("first run");
    }

    for (;;) {
        log_line(
#if CONFIG_IDF6TEST_WITH_HOSTED
            "Enter: run again   W: connect WiFi, then run again   F1/Esc: back to launcher"
#else
            "Enter: run again   F1/Esc: back to launcher"
#endif
        );
        bool waiting = true;
        while (waiting) {
            bsp_input_event_t event;
            if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;
            if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1 ||
                    event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                    bsp_device_restart_to_launcher();
                }
                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN && sdcard_is_mounted()) {
                    run_tests("run again");
                    waiting = false;
                }
            }
#if CONFIG_IDF6TEST_WITH_HOSTED
            if (event.type == INPUT_EVENT_TYPE_KEYBOARD &&
                (event.args_keyboard.ascii == 'w' || event.args_keyboard.ascii == 'W') && sdcard_is_mounted()) {
                if (connect_wifi()) {
                    run_tests("with WiFi connected");
                }
                waiting = false;
            }
#endif
        }
    }
}
