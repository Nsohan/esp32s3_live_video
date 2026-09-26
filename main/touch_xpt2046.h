#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// ─── Touch Pin Configuration (XPT2046 on Red 2.4" TFT) ───
#define TOUCH_PIN_IRQ   3    // T_IRQ (Pen Interrupt)
#define TOUCH_PIN_MISO  39   // T_DO  (Touch Data Out / MISO)
#define TOUCH_PIN_MOSI  41   // T_DIN (Touch Data In / MOSI - Shared with LCD)
#define TOUCH_PIN_CLK   40   // T_CLK (Touch Clock - Shared with LCD)
#define TOUCH_PIN_CS    38   // T_CS  (Touch Chip Select)

// ─── Touch Panel Calibration Settings ─────────────────────
// 2.4" TFT ILI9341 in Landscape Mode (320x240):
#define TOUCH_SWAP_XY       false  // Sensor X maps to Screen X, Sensor Y maps to Screen Y
#define TOUCH_INVERT_X      false  // Left is Left, Right is Right
#define TOUCH_INVERT_Y      true   // Top is Top, Bottom is Bottom

#define TOUCH_RAW_X_MIN     240
#define TOUCH_RAW_X_MAX     3800
#define TOUCH_RAW_Y_MIN     240
#define TOUCH_RAW_Y_MAX     3850

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize XPT2046 touch controller pins and SPI device
 */
esp_err_t touch_init(void);

/**
 * @brief Check if display is currently being touched
 */
bool touch_is_touched(void);

/**
 * @brief Read calibrated touch coordinates (Landscape 320x240)
 * @param x Output X coordinate (0 - 319)
 * @param y Output Y coordinate (0 - 239)
 * @return true if valid touch point detected, false if not touched
 */
bool touch_read(int *x, int *y);

/**
 * @brief Read raw 12-bit ADC values (useful for diagnostics & calibration)
 */
bool touch_read_raw(uint16_t *raw_x, uint16_t *raw_y);

/**
 * @brief Read both mapped coordinates and raw ADC values simultaneously
 */
bool touch_read_all(int *out_x, int *out_y, uint16_t *out_rx, uint16_t *out_ry);

#ifdef __cplusplus
}
#endif
