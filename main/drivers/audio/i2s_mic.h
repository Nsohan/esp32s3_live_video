#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// ─── INMP441 I2S Microphone Pin Configuration (I2S_NUM_1, RX only) ───
#define I2S_MIC_PIN_SCK   45   // Bit clock  (ESP32 output)
#define I2S_MIC_PIN_WS    46   // Word select (ESP32 output)
#define I2S_MIC_PIN_SD     0   // Data from mic (ESP32 input)

#define I2S_MIC_SAMPLE_RATE  16000

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Init INMP441 on I2S_NUM_1 (16 kHz, mono, left slot) and start it. */
esp_err_t i2s_mic_init(void);

/** @brief Read up to `samples` 16-bit mono samples. Blocks up to timeout_ms. */
esp_err_t i2s_mic_read(int16_t *dst, size_t samples, size_t *samples_read, uint32_t timeout_ms);

/** @brief Start a background task that logs the mic loudness (RMS) to serial. */
void i2s_mic_start_level_monitor(void);

void i2s_mic_deinit(void);

#ifdef __cplusplus
}
#endif