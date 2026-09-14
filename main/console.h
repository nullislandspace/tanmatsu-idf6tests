// Debug-console command listener, after tanmatsu-fonttest's bench_console.
//
// Reads newline-terminated commands from the USB-serial/JTAG peripheral through
// the driver API, never through stdin: mixing the driver with stdin loses bytes.
// printf/ESP_LOG output keeps using the normal console path.
//
// Commands:
//   PING               answered with a PONG record by the listener itself
//   RUN <suite> [k=v]  queued for the main task
//   EXIT               queued for the main task
//
// While idle the listener emits a READY record every 2 s. With no command for
// CONSOLE_IDLE_TIMEOUT_MS it queues EXIT, so an unattended cycle that failed on
// the host side still hands the badge back to the launcher.

#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CONSOLE_LINE_MAX        256
#define CONSOLE_IDLE_TIMEOUT_MS (5 * 60 * 1000)

typedef struct {
    char line[CONSOLE_LINE_MAX];  // "RUN smoke size=4096" or "EXIT"
} console_cmd_t;

// Start the listener. Commands other than PING are posted to the returned queue.
QueueHandle_t console_start(void);

// Stop the READY banner while a suite runs; resume it afterwards.
void console_set_busy(bool busy);
