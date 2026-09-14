#include "console.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "report.h"
#include "sdcard.h"
#include "sdkconfig.h"

static char const TAG[] = "console";

#define READ_CHUNK    64
#define BANNER_PERIOD pdMS_TO_TICKS(2000)

static QueueHandle_t s_queue         = NULL;
static volatile bool s_busy          = false;
static int64_t       s_idle_deadline = 0;

static void emit_hello(char const* kind) {
    int64_t left = (s_idle_deadline - esp_timer_get_time()) / 1000000;
    report_emitf(kind,
                 "{\"t\":\"%s\",\"variant\":\"%s\",\"mounted\":%s,\"idle_left_s\":%lld,"
                 "\"free_int\":%u,\"free_psram\":%u}",
                 kind, CONFIG_IDF6TEST_VARIANT_NAME, sdcard_is_mounted() ? "true" : "false",
                 (long long)(left > 0 ? left : 0), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void post(char const* line) {
    console_cmd_t cmd;
    strlcpy(cmd.line, line, sizeof(cmd.line));
    xQueueSend(s_queue, &cmd, portMAX_DELAY);
}

static void handle_line(char const* line) {
    if (line[0] == '\0') return;
    s_idle_deadline = esp_timer_get_time() + (int64_t)CONSOLE_IDLE_TIMEOUT_MS * 1000;
    if (strcmp(line, "PING") == 0) {
        emit_hello("PONG");
        return;
    }
    if (strncmp(line, "RUN ", 4) == 0 || strcmp(line, "EXIT") == 0) {
        // Stop the banner right away, before the main task picks the command up.
        s_busy = true;
        post(line);
        return;
    }
    ESP_LOGW(TAG, "Unknown command: %s", line);
}

static void console_task(void* arg) {
    (void)arg;
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 1024,
    };
    esp_err_t res = usb_serial_jtag_driver_install(&config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB-serial/JTAG driver: %d", res);
        vTaskDelete(NULL);
        return;
    }
    s_idle_deadline = esp_timer_get_time() + (int64_t)CONSOLE_IDLE_TIMEOUT_MS * 1000;

    char   line[CONSOLE_LINE_MAX];
    size_t len = 0;
    for (;;) {
        uint8_t buf[READ_CHUNK];
        int     n = usb_serial_jtag_read_bytes(buf, sizeof(buf), BANNER_PERIOD);
        if (n <= 0) {
            if (s_busy) continue;
            if (esp_timer_get_time() > s_idle_deadline) {
                ESP_LOGW(TAG, "No command for %d s, returning to launcher", CONSOLE_IDLE_TIMEOUT_MS / 1000);
                s_busy = true;
                post("EXIT");
                continue;
            }
            emit_hello("READY");
            continue;
        }
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                handle_line(line);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;  // overlong line: drop it, resync at the next newline
            }
        }
    }
}

QueueHandle_t console_start(void) {
    s_queue = xQueueCreate(4, sizeof(console_cmd_t));
    xTaskCreate(console_task, "console", 4096, NULL, 4, NULL);
    return s_queue;
}

void console_set_busy(bool busy) {
    if (!busy) {
        s_idle_deadline = esp_timer_get_time() + (int64_t)CONSOLE_IDLE_TIMEOUT_MS * 1000;
    }
    s_busy = busy;
}
