// gl_hidhost.c - USB HID Boot Keyboard support for the graceloader.
//
// Forked from hfmanson/hidhost 1.0.0 (MIT-licensed, by Hans Mansson).
// https://components.espressif.com/components/hfmanson/hidhost
//
// Changes vs. upstream:
//   * Per-keypress, inject NAVIGATION events (arrow keys, ENTER, ESC,
//     BACKSPACE, TAB, F1..F12, HOME/END/PGUP/PGDN, SUPER) in addition
//     to the SCANCODE events upstream already emits. Synthesises a
//     release event when the modifier-driven shifted bit goes away.
//   * Per-keypress, inject KEYBOARD ASCII events for printable keys
//     (letters, digits row, space, punctuation) honouring the
//     Shift modifier. US layout only for now.
//   * Track which HID scancodes are currently held + the modifier byte,
//     so gl_input_read_scancode / gl_input_read_navigation_key can
//     OR-merge USB state into the BSP's native-only polled API.
//   * Public API renamed to gl_hidhost_* and a one-shot, idempotent
//     init that spawns its own pump task — apps and the loader don't
//     need to call handle_hid_events() themselves.

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "bsp/input.h"
#include "bsp/power.h"

#include "gl_hidhost.h"

static const char *TAG = "gl_hidhost";

// ---- HID -> BSP scancode mapping ------------------------------------
//
// BSP scancodes don't fit in a byte: the escaped codes (arrows, RCTRL,
// RALT, meta keys, ...) are 16-bit (0xe0xx). Storing them in a 256-entry
// uint16_t table indexed by HID Usage ID keeps the lookup branchless.

static const uint16_t hid_to_bsp_scancode[256] = {
    [0x04] = BSP_INPUT_SCANCODE_A,
    [0x05] = BSP_INPUT_SCANCODE_B,
    [0x06] = BSP_INPUT_SCANCODE_C,
    [0x07] = BSP_INPUT_SCANCODE_D,
    [0x08] = BSP_INPUT_SCANCODE_E,
    [0x09] = BSP_INPUT_SCANCODE_F,
    [0x0A] = BSP_INPUT_SCANCODE_G,
    [0x0B] = BSP_INPUT_SCANCODE_H,
    [0x0C] = BSP_INPUT_SCANCODE_I,
    [0x0D] = BSP_INPUT_SCANCODE_J,
    [0x0E] = BSP_INPUT_SCANCODE_K,
    [0x0F] = BSP_INPUT_SCANCODE_L,
    [0x10] = BSP_INPUT_SCANCODE_M,
    [0x11] = BSP_INPUT_SCANCODE_N,
    [0x12] = BSP_INPUT_SCANCODE_O,
    [0x13] = BSP_INPUT_SCANCODE_P,
    [0x14] = BSP_INPUT_SCANCODE_Q,
    [0x15] = BSP_INPUT_SCANCODE_R,
    [0x16] = BSP_INPUT_SCANCODE_S,
    [0x17] = BSP_INPUT_SCANCODE_T,
    [0x18] = BSP_INPUT_SCANCODE_U,
    [0x19] = BSP_INPUT_SCANCODE_V,
    [0x1A] = BSP_INPUT_SCANCODE_W,
    [0x1B] = BSP_INPUT_SCANCODE_X,
    [0x1C] = BSP_INPUT_SCANCODE_Y,
    [0x1D] = BSP_INPUT_SCANCODE_Z,

    [0x1E] = BSP_INPUT_SCANCODE_1,
    [0x1F] = BSP_INPUT_SCANCODE_2,
    [0x20] = BSP_INPUT_SCANCODE_3,
    [0x21] = BSP_INPUT_SCANCODE_4,
    [0x22] = BSP_INPUT_SCANCODE_5,
    [0x23] = BSP_INPUT_SCANCODE_6,
    [0x24] = BSP_INPUT_SCANCODE_7,
    [0x25] = BSP_INPUT_SCANCODE_8,
    [0x26] = BSP_INPUT_SCANCODE_9,
    [0x27] = BSP_INPUT_SCANCODE_0,

    [0x28] = BSP_INPUT_SCANCODE_ENTER,
    [0x29] = BSP_INPUT_SCANCODE_ESC,
    [0x2A] = BSP_INPUT_SCANCODE_BACKSPACE,
    [0x2B] = BSP_INPUT_SCANCODE_TAB,
    [0x2C] = BSP_INPUT_SCANCODE_SPACE,
    [0x2D] = BSP_INPUT_SCANCODE_MINUS,
    [0x2E] = BSP_INPUT_SCANCODE_EQUAL,
    [0x2F] = BSP_INPUT_SCANCODE_LEFTBRACE,
    [0x30] = BSP_INPUT_SCANCODE_RIGHTBRACE,
    [0x31] = BSP_INPUT_SCANCODE_BACKSLASH,
    [0x33] = BSP_INPUT_SCANCODE_SEMICOLON,
    [0x34] = BSP_INPUT_SCANCODE_APOSTROPHE,
    [0x35] = BSP_INPUT_SCANCODE_GRAVE,
    [0x36] = BSP_INPUT_SCANCODE_COMMA,
    [0x37] = BSP_INPUT_SCANCODE_DOT,
    [0x38] = BSP_INPUT_SCANCODE_SLASH,

    [0x39] = BSP_INPUT_SCANCODE_CAPSLOCK,

    [0x3A] = BSP_INPUT_SCANCODE_F1,
    [0x3B] = BSP_INPUT_SCANCODE_F2,
    [0x3C] = BSP_INPUT_SCANCODE_F3,
    [0x3D] = BSP_INPUT_SCANCODE_F4,
    [0x3E] = BSP_INPUT_SCANCODE_F5,
    [0x3F] = BSP_INPUT_SCANCODE_F6,
    [0x40] = BSP_INPUT_SCANCODE_F7,
    [0x41] = BSP_INPUT_SCANCODE_F8,
    [0x42] = BSP_INPUT_SCANCODE_F9,
    [0x43] = BSP_INPUT_SCANCODE_F10,
    [0x44] = BSP_INPUT_SCANCODE_F11,
    [0x45] = BSP_INPUT_SCANCODE_F12,

    // Arrows and the grey-cluster keys are escaped scancodes.
    [0x49] = BSP_INPUT_SCANCODE_ESCAPED_GREY_INSERT,
    [0x4A] = BSP_INPUT_SCANCODE_ESCAPED_GREY_HOME,
    [0x4B] = BSP_INPUT_SCANCODE_ESCAPED_GREY_PGUP,
    [0x4C] = BSP_INPUT_SCANCODE_ESCAPED_GREY_DEL,
    [0x4D] = BSP_INPUT_SCANCODE_ESCAPED_GREY_END,
    [0x4E] = BSP_INPUT_SCANCODE_ESCAPED_GREY_PGDN,
    [0x4F] = BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT,
    [0x50] = BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT,
    [0x51] = BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN,
    [0x52] = BSP_INPUT_SCANCODE_ESCAPED_GREY_UP,

    // Numeric keypad
    [0x53] = BSP_INPUT_SCANCODE_NUMLOCK,
    [0x54] = BSP_INPUT_SCANCODE_SLASH,           // KP /
    [0x55] = BSP_INPUT_SCANCODE_KPASTERISK,
    [0x56] = BSP_INPUT_SCANCODE_KPMINUS,
    [0x57] = BSP_INPUT_SCANCODE_KPPLUS,
    [0x58] = BSP_INPUT_SCANCODE_ESCAPED_KPENTER,
    [0x59] = BSP_INPUT_SCANCODE_KP1,
    [0x5A] = BSP_INPUT_SCANCODE_KP2,
    [0x5B] = BSP_INPUT_SCANCODE_KP3,
    [0x5C] = BSP_INPUT_SCANCODE_KP4,
    [0x5D] = BSP_INPUT_SCANCODE_KP5,
    [0x5E] = BSP_INPUT_SCANCODE_KP6,
    [0x5F] = BSP_INPUT_SCANCODE_KP7,
    [0x60] = BSP_INPUT_SCANCODE_KP8,
    [0x61] = BSP_INPUT_SCANCODE_KP9,
    [0x62] = BSP_INPUT_SCANCODE_KP0,
    [0x63] = BSP_INPUT_SCANCODE_KPDOT,
    // 0x65 APPLICATION ("menu") - escaped menu key
    [0x65] = BSP_INPUT_SCANCODE_ESCAPED_MENU,
};

// ---- HID -> BSP navigation key mapping ------------------------------

static const uint8_t hid_to_bsp_nav[256] = {
    [0x28] = BSP_INPUT_NAVIGATION_KEY_RETURN,
    [0x29] = BSP_INPUT_NAVIGATION_KEY_ESC,
    [0x2A] = BSP_INPUT_NAVIGATION_KEY_BACKSPACE,
    [0x2B] = BSP_INPUT_NAVIGATION_KEY_TAB,
    [0x2C] = BSP_INPUT_NAVIGATION_KEY_SPACE_M,

    [0x3A] = BSP_INPUT_NAVIGATION_KEY_F1,
    [0x3B] = BSP_INPUT_NAVIGATION_KEY_F2,
    [0x3C] = BSP_INPUT_NAVIGATION_KEY_F3,
    [0x3D] = BSP_INPUT_NAVIGATION_KEY_F4,
    [0x3E] = BSP_INPUT_NAVIGATION_KEY_F5,
    [0x3F] = BSP_INPUT_NAVIGATION_KEY_F6,
    [0x40] = BSP_INPUT_NAVIGATION_KEY_F7,
    [0x41] = BSP_INPUT_NAVIGATION_KEY_F8,
    [0x42] = BSP_INPUT_NAVIGATION_KEY_F9,
    [0x43] = BSP_INPUT_NAVIGATION_KEY_F10,
    [0x44] = BSP_INPUT_NAVIGATION_KEY_F11,
    [0x45] = BSP_INPUT_NAVIGATION_KEY_F12,

    [0x4A] = BSP_INPUT_NAVIGATION_KEY_HOME,
    [0x4B] = BSP_INPUT_NAVIGATION_KEY_PGUP,
    [0x4D] = BSP_INPUT_NAVIGATION_KEY_END,
    [0x4E] = BSP_INPUT_NAVIGATION_KEY_PGDN,
    [0x4F] = BSP_INPUT_NAVIGATION_KEY_RIGHT,
    [0x50] = BSP_INPUT_NAVIGATION_KEY_LEFT,
    [0x51] = BSP_INPUT_NAVIGATION_KEY_DOWN,
    [0x52] = BSP_INPUT_NAVIGATION_KEY_UP,

    [0x65] = BSP_INPUT_NAVIGATION_KEY_MENU,

    // Meta keys (HID 0xE3 / 0xE7) deliberately not here — they arrive
    // through the modifier byte, not the regular keypress slots.
};

// ---- HID -> ASCII mapping (US layout) -------------------------------

static const char hid_to_ascii_unshifted[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',

    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',

    [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']',
    [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'', [0x35] = '`',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

static const char hid_to_ascii_shifted[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',

    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$',
    [0x22] = '%', [0x23] = '^', [0x24] = '&', [0x25] = '*',
    [0x26] = '(', [0x27] = ')',

    [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+',
    [0x2F] = '{', [0x30] = '}',
    [0x31] = '|',
    [0x33] = ':', [0x34] = '"', [0x35] = '~',
    [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

// ---- Modifier bit -> BSP scancode + flag mapping --------------------
//
// The HID modifier byte packs the eight modifier keys (L/R Ctrl,
// L/R Shift, L/R Alt, L/R Meta) into one bit each. We translate each
// bit edge into a SCANCODE event and into the matching BSP modifier
// flag bit (which the BSP defines independently).

typedef struct {
    uint8_t              hid_bit;
    bsp_input_scancode_t scancode;
    uint32_t             bsp_modifier;
} modifier_map_t;

static const modifier_map_t modifier_map[] = {
    { 0x01, BSP_INPUT_SCANCODE_LEFTCTRL,           BSP_INPUT_MODIFIER_CTRL_L  },
    { 0x02, BSP_INPUT_SCANCODE_LEFTSHIFT,          BSP_INPUT_MODIFIER_SHIFT_L },
    { 0x04, BSP_INPUT_SCANCODE_LEFTALT,            BSP_INPUT_MODIFIER_ALT_L   },
    { 0x08, BSP_INPUT_SCANCODE_ESCAPED_LEFTMETA,   BSP_INPUT_MODIFIER_SUPER_L },
    { 0x10, BSP_INPUT_SCANCODE_ESCAPED_RCTRL,      BSP_INPUT_MODIFIER_CTRL_R  },
    { 0x20, BSP_INPUT_SCANCODE_RIGHTSHIFT,         BSP_INPUT_MODIFIER_SHIFT_R },
    { 0x40, BSP_INPUT_SCANCODE_ESCAPED_RALT,       BSP_INPUT_MODIFIER_ALT_R   },
    { 0x80, BSP_INPUT_SCANCODE_ESCAPED_RIGHTMETA,  BSP_INPUT_MODIFIER_SUPER_R },
};

static uint32_t modifier_byte_to_bsp(uint8_t mod) {
    uint32_t out = 0;
    for (size_t i = 0; i < sizeof(modifier_map) / sizeof(modifier_map[0]); i++) {
        if (mod & modifier_map[i].hid_bit) {
            out |= modifier_map[i].bsp_modifier;
        }
    }
    return out;
}

// ---- Held-state mirror, read by gl_input polled API -----------------

static portMUX_TYPE held_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      held_keys[HID_KEYBOARD_KEY_MAX] = {0};
static uint8_t      held_modifier = 0;

bool gl_hidhost_scancode_held(bsp_input_scancode_t key) {
    if (key == BSP_INPUT_SCANCODE_NONE) {
        return false;
    }

    bool found = false;
    portENTER_CRITICAL(&held_lock);

    // Modifiers are tracked in the HID modifier byte, not the slot array.
    for (size_t i = 0; i < sizeof(modifier_map) / sizeof(modifier_map[0]); i++) {
        if ((held_modifier & modifier_map[i].hid_bit) &&
            modifier_map[i].scancode == key) {
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
            uint8_t hid = held_keys[i];
            if (hid > HID_KEY_ERROR_UNDEFINED &&
                hid_to_bsp_scancode[hid] == (uint16_t)key) {
                found = true;
                break;
            }
        }
    }

    portEXIT_CRITICAL(&held_lock);
    return found;
}

bool gl_hidhost_navigation_held(bsp_input_navigation_key_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_NONE) {
        return false;
    }

    bool found = false;
    portENTER_CRITICAL(&held_lock);
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t hid = held_keys[i];
        if (hid > HID_KEY_ERROR_UNDEFINED &&
            hid_to_bsp_nav[hid] == (uint8_t)key) {
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&held_lock);
    return found;
}

// ---- Event injection helpers ----------------------------------------

static void inject_scancode(bsp_input_scancode_t sc, bool pressed) {
    if (sc == BSP_INPUT_SCANCODE_NONE) {
        return;
    }
    bsp_input_event_t ev = {
        .type                   = INPUT_EVENT_TYPE_SCANCODE,
        .args_scancode.scancode = sc | (pressed ? 0 : BSP_INPUT_SCANCODE_RELEASE_MODIFIER),
    };
    bsp_input_inject_event(&ev);
}

static void inject_navigation(bsp_input_navigation_key_t key, bool pressed, uint32_t modifiers) {
    if (key == BSP_INPUT_NAVIGATION_KEY_NONE) {
        return;
    }
    bsp_input_event_t ev = {
        .type                       = INPUT_EVENT_TYPE_NAVIGATION,
        .args_navigation.key        = key,
        .args_navigation.modifiers  = modifiers,
        .args_navigation.state      = pressed,
    };
    bsp_input_inject_event(&ev);
}

static void inject_ascii(char ascii, uint32_t modifiers) {
    if (ascii == 0) {
        return;
    }
    // utf8 is an inline char[7] in the BSP definition; for plain ASCII the
    // UTF-8 encoding is the same single byte.
    bsp_input_event_t ev = {
        .type                    = INPUT_EVENT_TYPE_KEYBOARD,
        .args_keyboard.ascii     = ascii,
        .args_keyboard.modifiers = modifiers,
    };
    ev.args_keyboard.utf8[0] = ascii;
    ev.args_keyboard.utf8[1] = '\0';
    bsp_input_inject_event(&ev);
}

static void on_key_press(uint8_t hid_sc, uint8_t modifier_byte) {
    uint32_t mods = modifier_byte_to_bsp(modifier_byte);

    inject_scancode((bsp_input_scancode_t)hid_to_bsp_scancode[hid_sc], true);

    bsp_input_navigation_key_t nav = (bsp_input_navigation_key_t)hid_to_bsp_nav[hid_sc];
    inject_navigation(nav, true, mods);

    bool shift = (modifier_byte & (0x02 | 0x20)) != 0;
    char ascii = shift ? hid_to_ascii_shifted[hid_sc] : hid_to_ascii_unshifted[hid_sc];
    inject_ascii(ascii, mods);
}

static void on_key_release(uint8_t hid_sc, uint8_t modifier_byte) {
    uint32_t mods = modifier_byte_to_bsp(modifier_byte);

    inject_scancode((bsp_input_scancode_t)hid_to_bsp_scancode[hid_sc], false);

    bsp_input_navigation_key_t nav = (bsp_input_navigation_key_t)hid_to_bsp_nav[hid_sc];
    inject_navigation(nav, false, mods);
    // No ASCII release event — the BSP's own keyboard path doesn't emit
    // one either; consumers see only press transitions for ASCII.
}

static void inject_modifier_changes(uint8_t prev, uint8_t curr) {
    for (size_t i = 0; i < sizeof(modifier_map) / sizeof(modifier_map[0]); i++) {
        bool was = (prev & modifier_map[i].hid_bit) != 0;
        bool now = (curr & modifier_map[i].hid_bit) != 0;
        if (was == now) {
            continue;
        }
        inject_scancode(modifier_map[i].scancode, now);
    }
}

// ---- HID host plumbing (largely upstream) ----------------------------

static QueueHandle_t app_event_queue = NULL;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t  event;
    void                    *arg;
} app_event_queue_t;

static const char *hid_proto_name_str[] = { "NONE", "KEYBOARD", "MOUSE" };

static inline bool key_in_set(const uint8_t *src, uint8_t key, unsigned length) {
    for (unsigned i = 0; i < length; i++) {
        if (src[i] == key) {
            return true;
        }
    }
    return false;
}

static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length) {
    if (length < (int)sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }
    const hid_keyboard_input_report_boot_t *kb = (const hid_keyboard_input_report_boot_t *)data;

    static uint8_t prev_modifier = 0;
    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = {0};

    uint8_t modifier = kb->modifier.val;
    inject_modifier_changes(prev_modifier, modifier);

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        // Released
        if (prev_keys[i] > HID_KEY_ERROR_UNDEFINED &&
            !key_in_set(kb->key, prev_keys[i], HID_KEYBOARD_KEY_MAX)) {
            on_key_release(prev_keys[i], modifier);
        }
        // Pressed
        if (kb->key[i] > HID_KEY_ERROR_UNDEFINED &&
            !key_in_set(prev_keys, kb->key[i], HID_KEYBOARD_KEY_MAX)) {
            on_key_press(kb->key[i], modifier);
        }
    }

    // Publish the new held-state for the polled API.
    portENTER_CRITICAL(&held_lock);
    memcpy(held_keys, kb->key, HID_KEYBOARD_KEY_MAX);
    held_modifier = modifier;
    portEXIT_CRITICAL(&held_lock);

    prev_modifier = modifier;
    memcpy(prev_keys, kb->key, HID_KEYBOARD_KEY_MAX);
}

static void hid_host_interface_callback(hid_host_device_handle_t dev,
                                        const hid_host_interface_event_t event,
                                        void *arg) {
    (void)arg;
    uint8_t                  data[64] = {0};
    size_t                   data_length = 0;
    hid_host_dev_params_t    dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(dev, &dev_params));

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(dev, data,
                                                                     sizeof(data),
                                                                     &data_length));
            if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE &&
                dev_params.proto     == HID_PROTOCOL_KEYBOARD) {
                hid_host_keyboard_report_callback(data, data_length);
            }
            // Mouse and generic devices: ignored for now (no gl_input
            // pointer surface yet). Plug-in extension point.
            break;

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID '%s' disconnected", hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(hid_host_device_close(dev));
            // Clear the held-state mirror so the polled API doesn't
            // report ghost-held keys after an unplug.
            portENTER_CRITICAL(&held_lock);
            memset(held_keys, 0, sizeof(held_keys));
            held_modifier = 0;
            portEXIT_CRITICAL(&held_lock);
            break;

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGI(TAG, "HID '%s' transfer error", hid_proto_name_str[dev_params.proto]);
            break;

        default:
            ESP_LOGW(TAG, "HID '%s' unhandled event %d",
                     hid_proto_name_str[dev_params.proto], event);
            break;
    }
}

static void hid_host_device_event(hid_host_device_handle_t dev,
                                  const hid_host_driver_event_t event,
                                  void *arg) {
    (void)arg;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(dev, &dev_params));

    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "HID '%s' connected", hid_proto_name_str[dev_params.proto]);

        const hid_host_device_config_t dev_config = {
            .callback     = hid_host_interface_callback,
            .callback_arg = NULL,
        };

        if (dev_params.proto != HID_PROTOCOL_NONE) {
            ESP_ERROR_CHECK(hid_host_device_open(dev, &dev_config));
            if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                ESP_ERROR_CHECK(hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT));
                if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                    ESP_ERROR_CHECK(hid_class_request_set_idle(dev, 0, 0));
                }
            }
            ESP_ERROR_CHECK(hid_host_device_start(dev));
        }
    }
}

static void usb_lib_task(void *arg) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB host shutdown");
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void hid_host_device_callback(hid_host_device_handle_t dev,
                                     const hid_host_driver_event_t event,
                                     void *arg) {
    const app_event_queue_t entry = { .handle = dev, .event = event, .arg = arg };
    if (app_event_queue) {
        xQueueSend(app_event_queue, &entry, 0);
    }
}

static void event_pump_task(void *arg) {
    (void)arg;
    while (true) {
        app_event_queue_t entry;
        if (xQueueReceive(app_event_queue, &entry, portMAX_DELAY) == pdTRUE) {
            hid_host_device_event(entry.handle, entry.event, entry.arg);
        }
    }
}

// ---- Public init -----------------------------------------------------

static bool initialized = false;

esp_err_t gl_hidhost_init(void) {
    if (initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Bringing up USB HID host");
    bsp_power_set_usb_host_boost_enabled(true);

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_events",
                                            4096, xTaskGetCurrentTaskHandle(),
                                            2, NULL, 0);
    if (ok != pdTRUE) {
        return ESP_FAIL;
    }

    // Wait for the USB host library to be ready.
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority          = 5,
        .stack_size             = 4096,
        .core_id                = 0,
        .callback               = hid_host_device_callback,
        .callback_arg           = NULL,
    };
    esp_err_t res = hid_host_install(&hid_cfg);
    if (res != ESP_OK) {
        return res;
    }

    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
    if (app_event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(event_pump_task, "gl_hid_pump",
                                 2560, NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        return ESP_FAIL;
    }

    initialized = true;
    ESP_LOGI(TAG, "USB HID host ready");
    return ESP_OK;
}
