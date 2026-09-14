// gl_input.c - Public merged input API.
//
// Each gl_input_* function delegates to the BSP for native state (which
// covers the on-board Tanmatsu coprocessor keyboard), and then ORs in
// whatever USB devices the gl_hidhost layer is currently tracking.

#include "gl_input.h"
#include "gl_hidhost.h"
#include "bsp/input.h"
#include "esp_log.h"

static const char *TAG = "gl_input";

esp_err_t gl_input_init(void) {
    esp_err_t res = gl_hidhost_init();
    if (res != ESP_OK) {
        // Non-fatal: a USB host failure shouldn't prevent the app from
        // booting and using the native keyboard. Log and continue.
        ESP_LOGW(TAG, "USB HID host bringup failed (%d) — native input only", res);
    }
    return ESP_OK;
}

esp_err_t gl_input_get_queue(QueueHandle_t* out_queue) {
    // USB and native events both feed the BSP queue (the native side
    // through the coprocessor driver, USB through bsp_input_inject_event
    // called from gl_hidhost). One queue, two producers.
    return bsp_input_get_queue(out_queue);
}

esp_err_t gl_input_read_navigation_key(bsp_input_navigation_key_t key, bool* out_state) {
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool native = false;
    esp_err_t res = bsp_input_read_navigation_key(key, &native);
    if (res != ESP_OK) {
        return res;
    }

    *out_state = native || gl_hidhost_navigation_held(key);
    return ESP_OK;
}

esp_err_t gl_input_read_scancode(bsp_input_scancode_t key, bool* out_state) {
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool native = false;
    esp_err_t res = bsp_input_read_scancode(key, &native);
    if (res != ESP_OK) {
        return res;
    }

    *out_state = native || gl_hidhost_scancode_held(key);
    return ESP_OK;
}

esp_err_t gl_input_read_action(bsp_input_action_type_t action, bool* out_state) {
    // USB devices don't generate action events (SD card, audio jack,
    // power button etc. are all hardware-local). Pass-through.
    return bsp_input_read_action(action, out_state);
}
