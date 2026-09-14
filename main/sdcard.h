#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"

// Initialize SD card power and mount filesystem at /sd
esp_err_t sdcard_init(void);

// Check if SD card is mounted
bool sdcard_is_mounted(void);

// The mounted card, or NULL.
sdmmc_card_t* sdcard_get_card(void);

// Use (as graceloader does) or drop the 2 KB internal bounce buffer in
// host.dma_aligned_buffer, without remounting.
void sdcard_set_dma_aligned_buffer(bool enable);
