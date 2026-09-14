// gl_hidhost.h - Internal interface between gl_input and the forked HID host.
//
// gl_hidhost owns the USB host stack, the HID host driver, and the
// per-device callbacks. It injects events into the BSP queue
// (SCANCODE + NAVIGATION + KEYBOARD), and it tracks the currently
// held HID keys so the polled API can OR-merge USB state with the
// native coprocessor state.
//
// This header is not exported to apps. Apps go through gl_input.h.

#pragma once

#include <stdbool.h>
#include "bsp/input.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up USB host + HID host. Spawns the usb_events task (Core 0,
// priority 2) and the small pump task that drives the HID host's
// device-connect/disconnect queue (Core 0, low priority). Idempotent —
// the second call is a no-op.
esp_err_t gl_hidhost_init(void);

// Return true if any currently-held USB key, when translated through
// our HID->BSP scancode map, matches the requested BSP scancode.
// Also covers modifier keys (LCTRL/LSHIFT/LALT/META/RCTRL/RSHIFT/RALT).
bool gl_hidhost_scancode_held(bsp_input_scancode_t key);

// Return true if any currently-held USB key maps to the requested
// BSP navigation key.
bool gl_hidhost_navigation_held(bsp_input_navigation_key_t key);

#ifdef __cplusplus
}
#endif
