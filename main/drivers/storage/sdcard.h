#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SDCARD_MOUNT_POINT   "/sdcard"
#define SDCARD_PIN_CS        14

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and mount MicroSD Card on the shared SPI bus (SPI2_HOST)
 * @return ESP_OK on success, or error code
 */
esp_err_t sdcard_init(void);

/**
 * @brief Check if the SD card is currently mounted and accessible
 */
bool sdcard_is_mounted(void);

/**
 * @brief Get total and free space on the SD Card in megabytes (MB)
 */
esp_err_t sdcard_get_space_mb(uint32_t *out_total_mb, uint32_t *out_free_mb);

/**
 * @brief List files in a specific SD Card directory to the console log
 */
void sdcard_print_directory(const char *dir_path);

/**
 * @brief Unmount SD Card safely
 */
void sdcard_unmount(void);

#ifdef __cplusplus
}
#endif
